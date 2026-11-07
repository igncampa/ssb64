//! Build the little-endian relocData region from the typed C sources.
//!
//! Each file compiles for an ILP32 little-endian target, one ELF section per symbol.
//! This tool lays the symbols out the way IDO did, in source order and word aligned,
//! and writes the ROM's chain words into the pointer slots in native order. The game's
//! lbreloc.c then walks them unchanged. Files are stored uncompressed.
//!
//! Each file is checked against the decompressed ROM file: size, chain offsets equal
//! the relocation offsets, and every aligned word equals the ROM's as bytes, swap32,
//! or per-halfword swap. Words that differ only because a bitfield struct initialised
//! by name lays its bits out LSB-first are allowed by symbol name, nothing else is.
use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::Path;
use std::process::Command;
use std::sync::{Arc, Mutex};

const CFLAGS: &[&str] = &[
    "-m32", "-malign-double", "-malign-data=abi", "-c", "-std=gnu89", "-w", "-O0", "-ffreestanding",
    "-fno-toplevel-reorder", "-fdata-sections", "-fno-common", "-fno-builtin", "-fpermissive", "-fno-pic",
    "-fno-pie", "-fno-zero-initialized-in-bss", "-D_LANGUAGE_C", "-DPORT", "-DAVOID_UB", "-DNON_MATCHING",
    "-DREGION_US", "-DF3DEX_GBI_2", "-D_MIPS_SZLONG=32", "-DNDEBUG", "-DN_MICRO", "-D_FINALROM", "-Iinclude",
    "-Isrc", "-Iport/include", "-Isrc/relocData", "-Ibuild/us/src/relocData", "-Ibuild/us/port/gen",
];
/// Bitfield structs initialised by name. Writer and reader agree, the bits just sit elsewhere.
const NAMED_BITFIELD: &[&str] = &["Attributes", "_attr", "AttackEvents", "HitParties", "dBossMainMotion", "ItemAttributes", "ITBonus1ObjectHeader"];
/// MIPS code stored as data. The port stubs its callers, so the bytes are never used.
const CODE_FILES: &[usize] = &[199, 200];

fn u16le(d: &[u8], o: usize) -> u16 { u16::from_le_bytes([d[o], d[o + 1]]) }
fn u32le(d: &[u8], o: usize) -> u32 { u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) }
fn cstr(d: &[u8], o: usize) -> String {
    let e = d[o..].iter().position(|&b| b == 0).unwrap();
    String::from_utf8_lossy(&d[o..o + e]).into_owned()
}

#[derive(Clone)]
struct Sec { name: String, ty: u32, off: usize, size: usize, link: usize, info: usize, align: usize }

fn sections(d: &[u8]) -> Vec<Sec> {
    let shoff = u32le(d, 32) as usize;
    let (shes, shn, shstr) = (u16le(d, 46) as usize, u16le(d, 48) as usize, u16le(d, 50) as usize);
    let strs = u32le(d, shoff + shstr * shes + 16) as usize;
    (0..shn).map(|i| {
        let b = shoff + i * shes;
        Sec { name: cstr(d, strs + u32le(d, b) as usize), ty: u32le(d, b + 4), off: u32le(d, b + 16) as usize,
              size: u32le(d, b + 20) as usize, link: u32le(d, b + 24) as usize, info: u32le(d, b + 28) as usize,
              align: u32le(d, b + 32) as usize }
    }).collect()
}

struct Obj { body: Vec<u8>, relocs: Vec<usize>, syms: Vec<(usize, String)>, sizes: HashMap<String, usize> }

/// Lay out .data sections in file order, each aligned to at least 4 like IDO.
fn read_obj(path: &str) -> Obj {
    let d = fs::read(path).unwrap();
    let secs = sections(&d);
    let mut place: HashMap<usize, usize> = HashMap::new();
    let mut body = Vec::new();
    for (i, s) in secs.iter().enumerate() {
        if s.ty == 1 && (s.name == ".data" || s.name.starts_with(".data.") || s.name == ".rodata" || s.name.starts_with(".rodata.")) {
            let a = s.align.max(4);
            while body.len() % a != 0 { body.push(0); }
            place.insert(i, body.len());
            body.extend_from_slice(&d[s.off..s.off + s.size]);
        }
    }
    let mut relocs = Vec::new();
    for s in &secs {
        if s.ty == 9 {
            if let Some(&base) = place.get(&s.info) {
                for k in 0..s.size / 8 { relocs.push(base + u32le(&d, s.off + k * 8) as usize); }
            }
        }
    }
    relocs.sort();
    let mut syms = Vec::new();
    let mut sizes = HashMap::new();
    if let Some(st) = secs.iter().find(|s| s.ty == 2) {
        let strs = secs[st.link].off;
        for k in 0..st.size / 16 {
            let b = st.off + k * 16;
            let shndx = u16le(&d, b + 14) as usize;
            if d[b + 12] & 15 == 1 {
                if let Some(&base) = place.get(&shndx) {
                    let name = cstr(&d, strs + u32le(&d, b) as usize);
                    sizes.insert(name.clone(), u32le(&d, b + 8) as usize);
                    syms.push((base + u32le(&d, b + 4) as usize, name));
                }
            }
        }
    }
    syms.sort();
    Obj { body, relocs, syms, sizes }
}

fn chain(rom: &[u8], start: u16) -> Vec<usize> {
    let mut out = Vec::new();
    let mut cur = start;
    while cur != 0xFFFF {
        let off = cur as usize * 4;
        out.push(off);
        cur = u16::from_be_bytes([rom[off], rom[off + 1]]);
    }
    out
}

fn word_ok(a: &[u8], r: &[u8]) -> bool {
    let rev = [r[3], r[2], r[1], r[0]];
    if a == r || a == rev { return true; }
    let h = |x: &[u8], y: &[u8]| x == y || (x[0] == y[1] && x[1] == y[0]);
    h(&a[..2], &r[..2]) && h(&a[2..], &r[2..])
}

fn main() {
    let root = std::env::args().nth(1).unwrap_or_else(|| ".".into());
    std::env::set_current_dir(&root).unwrap();
    let objdir = "build/us/port/relocobj";
    fs::create_dir_all(objdir).unwrap();

    // file table from the ROM extract
    let csv = fs::read_to_string("assets/us/relocData.csv").unwrap();
    let tab: Vec<(u16, u16)> = csv.lines().skip(1).map(|l| {
        let f: Vec<&str> = l.split(',').map(|x| x.trim()).collect();
        (u16::from_str_radix(f[2].trim_start_matches("0x"), 16).unwrap(),
         u16::from_str_radix(f[4].trim_start_matches("0x"), 16).unwrap())
    }).collect();

    // masters: src/relocData/<id>_<name>.c or a spritelist-generated master under build/
    let mut rows: BTreeMap<usize, (String, String)> = BTreeMap::new();
    for e in fs::read_dir("src/relocData").unwrap() {
        let n = e.unwrap().file_name().to_string_lossy().into_owned();
        let Some((id, rest)) = n.split_once('_') else { continue };
        let Ok(id) = id.parse::<usize>() else { continue };
        if let Some(base) = rest.strip_suffix(".c") {
            rows.insert(id, (base.into(), format!("src/relocData/{n}")));
        } else if let Some(base) = rest.strip_suffix(".spritelist") {
            let dst = format!("build/us/src/relocData/{id}_{base}.c");
            if !Path::new(&dst).exists() {
                let py = if Path::new(".venv/bin/python3").exists() { ".venv/bin/python3" } else { "python3" };
                let st = Command::new(py).args(["tools/extractSpriteFile.py", &id.to_string(), &dst, "--version", "us"])
                    .output().unwrap();
                assert!(st.status.success(), "{}", String::from_utf8_lossy(&st.stderr));
            }
            rows.insert(id, (base.into(), dst));
        }
    }
    assert!(rows.len() + 1 >= tab.len(), "one source per table entry");

    // compile
    let queue = Arc::new(Mutex::new(rows.iter().map(|(k, v)| (*k, v.1.clone())).collect::<Vec<_>>()));
    let fails = Arc::new(Mutex::new(Vec::new()));
    let hs: Vec<_> = (0..16).map(|_| {
        let (queue, fails) = (queue.clone(), fails.clone());
        std::thread::spawn(move || loop {
            let Some((id, src)) = queue.lock().unwrap().pop() else { break };
            let o = format!("build/us/port/relocobj/{id}.o");
            let fresh = fs::metadata(&o).and_then(|m| m.modified()).ok()
                .zip(fs::metadata(&src).and_then(|m| m.modified()).ok()).map(|(a, b)| a > b).unwrap_or(false);
            if fresh { continue; }
            let r = Command::new("gcc").args(CFLAGS).args(["-o", &o, &src]).output().unwrap();
            if !r.status.success() {
                fails.lock().unwrap().push((id, String::from_utf8_lossy(&r.stderr).chars().take(300).collect::<String>()));
            }
        })
    }).collect();
    for h in hs { h.join().unwrap(); }
    for (id, e) in fails.lock().unwrap().iter() { eprintln!("compile {id}: {e}"); }

    // verify and emit
    let (mut size_bad, mut chain_bad, mut word_bad, mut named) = (0, 0, 0usize, 0usize);
    let mut image: Vec<Vec<u8>> = Vec::new();
    let mut rawlog = String::new();
    let mut u32_blocks = 0;
    let animjoint_files = animjoint_file_ids();
    let rdp_blocks = rdp_block_names(rows.values().map(|v| v.1.as_str()));
    let mut rdp_count = 0;
    println!("RDP-read blocks named in sources: {}", rdp_blocks.len());
    println!("AnimJoint fighter animations: {}", animjoint_files.len());
    for (id, (name, _)) in &rows {
        let rom = fs::read(format!("assets/us/relocData/{id}.vpk0.bin"))
            .or_else(|_| fs::read(format!("assets/us/relocData/{id}.bin"))).unwrap();
        let mut obj = read_obj(&format!("{objdir}/{id}.o"));
        if CODE_FILES.contains(id) { image.push(vec![0; rom.len()]); continue; }
        if obj.body.len() < rom.len() && rom.len() - obj.body.len() < 16 && rom[obj.body.len()..].iter().all(|&b| b == 0) {
            obj.body.resize(rom.len(), 0); // IDO rounds .data up to 16 bytes
        }
        if obj.body.len() != rom.len() {
            println!("SIZE {id} {name}: compiled {} rom {}", obj.body.len(), rom.len());
            size_bad += 1; image.push(vec![]); continue;
        }
        let (ci, ce) = tab[*id];
        let mut want = if ci != 0xFFFF { chain(&rom, ci) } else { vec![] };
        if ce != 0xFFFF { want.extend(chain(&rom, ce)); }
        want.sort();
        if want != obj.relocs {
            let w: std::collections::BTreeSet<_> = want.iter().collect();
            let r: std::collections::BTreeSet<_> = obj.relocs.iter().collect();
            let diff: Vec<_> = w.symmetric_difference(&r).take(4).collect();
            println!("CHAIN {id} {name}: want {} got {} first {:x?}", want.len(), obj.relocs.len(), diff);
            chain_bad += 1;
        }
        let slots: std::collections::HashSet<usize> = want.iter().cloned().collect();
        let mut out = obj.body.clone();
        let mut bysym: BTreeMap<String, (usize, usize)> = BTreeMap::new();
        let mut i = 0;
        // Report blocks kept as raw big-endian bytes: fine for pixels, a bug if read as words.
        let mut raw: BTreeMap<String, usize> = BTreeMap::new();
        while i + 4 <= rom.len() {
            if !slots.contains(&i) && obj.body[i..i + 4] == rom[i..i + 4] && (rom[i] != rom[i + 3] || rom[i + 1] != rom[i + 2]) {
                let idx = obj.syms.partition_point(|s| s.0 <= i);
                let s = if idx > 0 { obj.syms[idx - 1].1.clone() } else { "?".into() };
                *raw.entry(s).or_insert(0) += 1;
            }
            i += 4;
        }
        for (s, n) in &raw { rawlog.push_str(&format!("{id} {name} {s} {n}\n")); }
        i = 0;
        while i + 4 <= rom.len() {
            if slots.contains(&i) {
                out[i..i + 4].copy_from_slice(&[rom[i + 3], rom[i + 2], rom[i + 1], rom[i]]);
            } else if !word_ok(&obj.body[i..i + 4], &rom[i..i + 4]) {
                let idx = obj.syms.partition_point(|s| s.0 <= i);
                let s = if idx > 0 { obj.syms[idx - 1].1.clone() } else { "?".into() };
                let e = bysym.entry(s).or_insert((0, i)); e.0 += 1;
            }
            i += 4;
        }
        for (s, (n, first)) in &bysym {
            if NAMED_BITFIELD.iter().any(|t| s.contains(t)) { named += n; continue; }
            word_bad += n;
            println!("WORD {id} {name} {s}: {n} words, first {first:#x} compiled {:02x?} rom {:02x?}",
                     &obj.body[*first..*first + 4], &rom[*first..*first + 4]);
        }
        // A few script blocks are typed u16 upstream. Big-endian cannot tell; here the halves would swap.
        let srcpath = &rows[id].1;
        let text = fs::read_to_string(srcpath).unwrap_or_default();
        for (off, sym) in &obj.syms {
            if !text.contains(&format!("(AObjEvent32 *){sym},")) && !text.contains(&format!("(AObjEvent32 *){sym}\n")) { continue; }
            let Some(decl) = text.find(&format!("u16 {sym}[")) else { continue };
            if !text[decl..(decl + 200).min(text.len())].contains(".palette.inc.c") { continue; }
            let size = obj.sizes[sym];
            for w in (*off..*off + size).step_by(4) { out[w..w + 4].copy_from_slice(&[rom[w + 3], rom[w + 2], rom[w + 1], rom[w]]); }
            u32_blocks += 1;
        }
        // Anything a display list loads as an image or palette, or a material lists as a palette, is read
        // only by the RDP, which sees big-endian texel memory. Keep its ROM bytes.
        for (off, sym) in &obj.syms {
            // Fighter animation files hold u16 pointer tables to their scripts, and no images at all.
            if !rdp_blocks.contains(sym) || (name.starts_with("FT") && name.contains("Anim")) { continue; }
            if text.contains(&format!("(AObjEvent32 *){sym},")) || text.contains(&format!("(AObjEvent32 *){sym}\n")) { continue; }
            let size = obj.sizes[sym];
            if *off + size > rom.len() { continue; }
            for i in 0..size {
                if slots.contains(&((*off + i) & !3)) { continue; }
                out[*off + i] = rom[*off + i];
            }
            rdp_count += 1;
        }
        // Fighter animations flagged FTANIM_FLAG_ANIMJOINT are read as AObjEvent32, but typed u16 like the rest.
        if animjoint_files.contains(id) {
            for (off, sym) in &obj.syms {
                if !sym.contains("_joint") || sym.ends_with("_joints") { continue; }
                let size = obj.sizes[sym];
                for w in (*off..*off + size).step_by(4) { out[w..w + 4].copy_from_slice(&[rom[w + 3], rom[w + 2], rom[w + 1], rom[w]]); }
                u32_blocks += 1;
            }
        }
        image.push(out);
    }
    println!("RDP-read blocks kept big-endian: {rdp_count}");
    println!("script blocks retyped to u32: {u32_blocks}");
    println!("files {} size_bad {size_bad} chain_bad {chain_bad} word_bad {word_bad} named_bitfield_words {named} compile_fail {}",
             rows.len(), fails.lock().unwrap().len());
    fs::write("build/us/port/raw_blocks.txt", &rawlog).unwrap();
    let total: usize = image.iter().map(|f| f.len()).sum();
    println!("uncompressed bytes {total}");
    if size_bad + chain_bad + word_bad + fails.lock().unwrap().len() != 0 { std::process::exit(1); }
    write_image(&image, &rows_externs(&tab, &rows), &tab);
}

/// File ids a file's extern chain points at, read from the ROM right after its compressed data.
fn rows_externs(tab: &[(u16, u16)], rows: &BTreeMap<usize, (String, String)>) -> Vec<Vec<u16>> {
    let rom = fs::read("baserom.us.z64").unwrap();
    let table = 0x1AC870usize;
    let n = tab.len() - 1;
    let hi = table + (n + 1) * 12;
    let mut out = vec![Vec::new(); n];
    for id in rows.keys() {
        let e = table + id * 12;
        let word = u32::from_be_bytes([rom[e], rom[e + 1], rom[e + 2], rom[e + 3]]);
        let next = u32::from_be_bytes([rom[e + 12], rom[e + 13], rom[e + 14], rom[e + 15]]) & 0x7FFFFFFF;
        let comp = u16::from_be_bytes([rom[e + 6], rom[e + 7]]) as usize * 4;
        let start = (word & 0x7FFFFFFF) as usize + comp;
        let mut o = hi + start;
        while o < hi + next as usize { out[*id].push(u16::from_be_bytes([rom[o], rom[o + 1]])); o += 2; }
    }
    out
}

const B1_S1_CTL_END: usize = 0xB54CE0;
// ROM regions, from smashbrothers.us.yaml.
const BANKS: &[(&str, usize, usize)] = &[
    ("efcommon", 0xAC7340, 0xAC9DE0), ("efcommon_txb", 0xAC9DE0, 0xB16C80),
    ("particles_unk0", 0xB16C80, 0xB17060), ("particles_unk0_txb", 0xB17060, 0xB174A0),
    ("particles_unk1", 0xB174A0, 0xB176A0), ("particles_unk1_txb", 0xB176A0, 0xB19700),
    ("particles_unk2", 0xB19700, 0xB19850), ("particles_unk2_txb", 0xB19850, 0xB1BCA0),
    ("itcommon", 0xB1BCA0, 0xB1BDE0), ("itcommon_txb", 0xB1BDE0, 0xB1E640),
    ("grpupupu", 0xB1E640, 0xB1E7E0), ("grpupupu_txb", 0xB1E7E0, 0xB1F960),
    ("grhyrule", 0xB1F960, 0xB1FC80), ("grhyrule_txb", 0xB1FC80, 0xB22980),
    ("gryoster", 0xB22980, 0xB22A00), ("gryoster_txb", 0xB22A00, 0xB22C30),
    ("mntitle", 0xB22C30, 0xB22D40), ("mntitle_txb", 0xB22D40, 0xB277B0),
];

fn swap32(d: &mut [u8], o: usize) { d[o..o + 4].reverse(); }
fn swap16(d: &mut [u8], o: usize) { d[o..o + 2].reverse(); }
fn be32(d: &[u8], o: usize) -> usize { u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as usize }
fn be16(d: &[u8], o: usize) -> usize { u16::from_be_bytes([d[o], d[o + 1]]) as usize }

/// Swap an instrument bank in place, walking it the way alBnkfNew does.
fn swap_ctl(d: &mut [u8]) {
    let banks = be16(d, 2);
    let mut seen = std::collections::HashSet::new();
    let bank_offs: Vec<usize> = (0..banks).map(|i| be32(d, 4 + i * 4)).collect();
    swap16(d, 0); swap16(d, 2);
    for i in 0..banks { swap32(d, 4 + i * 4); }
    for b in bank_offs {
        if b == 0 || !seen.insert(b) { continue; }
        let count = be16(d, b);
        let perc = be32(d, b + 8);
        let insts: Vec<usize> = (0..count).map(|i| be32(d, b + 12 + i * 4)).collect();
        swap16(d, b); swap32(d, b + 4); swap32(d, b + 8);
        for i in 0..count { swap32(d, b + 12 + i * 4); }
        for inst in std::iter::once(perc).chain(insts) {
            if inst == 0 || !seen.insert(inst) { continue; }
            let sc = be16(d, inst + 14);
            let sounds: Vec<usize> = (0..sc).map(|i| be32(d, inst + 16 + i * 4)).collect();
            swap16(d, inst + 12); swap16(d, inst + 14);
            for i in 0..sc { swap32(d, inst + 16 + i * 4); }
            for s in sounds {
                if s == 0 || !seen.insert(s) { continue; }
                let (env, wave) = (be32(d, s), be32(d, s + 8));
                swap32(d, s); swap32(d, s + 4); swap32(d, s + 8);
                if seen.insert(env) { for k in 0..3 { swap32(d, env + k * 4); } }
                if !seen.insert(wave) { continue; }
                let ty = d[wave + 8];
                let (lp, book) = (be32(d, wave + 12), be32(d, wave + 16));
                swap32(d, wave); swap32(d, wave + 4); swap32(d, wave + 12);
                if ty == 0 {
                    swap32(d, wave + 16);
                    if book != 0 && seen.insert(book) {
                        let n = be32(d, book) * be32(d, book + 4) * 8;
                        swap32(d, book); swap32(d, book + 4);
                        for k in 0..n { swap16(d, book + 8 + k * 2); }
                    }
                    if lp != 0 && seen.insert(lp) {
                        for k in 0..3 { swap32(d, lp + k * 4); }
                        for k in 0..16 { swap16(d, lp + 12 + k * 2); }
                    }
                } else if lp != 0 && seen.insert(lp) {
                    for k in 0..3 { swap32(d, lp + k * 4); }
                }
            }
        }
    }
}

fn compile_bank(name: &str, rom: &[u8]) -> Vec<u8> {
    let fname = if name.ends_with("_txb") { name.to_string() } else { format!("{name}_scb") };
    let src = format!("src/particles/{fname}.c");
    let o = format!("build/us/port/relocobj/{fname}.o");
    let r = Command::new("gcc").args(CFLAGS).args(["-Ibuild/us/src", "-Ibuild/us/src/particles", "-o", &o, &src]).output().unwrap();
    assert!(r.status.success(), "{src}: {}", String::from_utf8_lossy(&r.stderr));
    let mut obj = read_obj(&o);
    if obj.body.len() < rom.len() && rom[obj.body.len()..].iter().all(|&b| b == 0) { obj.body.resize(rom.len(), 0); }
    assert_eq!(obj.body.len(), rom.len(), "{src}: size");
    assert!(obj.relocs.is_empty(), "{src}: banks hold offsets, not pointers");
    let mut bad = 0;
    for i in (0..rom.len() & !3).step_by(4) { if !word_ok(&obj.body[i..i + 4], &rom[i..i + 4]) { bad += 1; } }
    println!("bank {name}: {} bytes, {bad} words differ", rom.len());
    assert_eq!(bad, 0, "{src}");
    obj.body
}

fn write_image(files: &[Vec<u8>], ext: &[Vec<u16>], tab: &[(u16, u16)]) {
    let rom = fs::read("baserom.us.z64").unwrap();
    let n = files.len();
    let mut img: Vec<u8> = vec![0; (n + 1) * 12];
    let hi = img.len();
    let mut entries = Vec::new();
    for (i, f) in files.iter().enumerate() {
        let start = img.len() - hi;
        img.extend_from_slice(f);
        assert!(f.len() % 4 == 0, "file {i} size");
        for id in &ext[i] { img.extend_from_slice(&id.to_le_bytes()); }
        while (img.len() - hi) % 4 != 0 { img.push(0); }
        entries.push((start, f.len()));
    }
    let end = img.len() - hi;
    for i in 0..=n {
        let e = i * 12;
        let (start, len) = if i < n { entries[i] } else { (end, 0) };
        // is_compressed is the top bit of the first word, data_offset the rest; both native here
        img[e..e + 4].copy_from_slice(&(start as u32).to_le_bytes());
        let (ci, ce) = if i < n { tab[i] } else { (0xFFFF, 0xFFFF) };
        let words = (len / 4) as u32;
        assert!(words <= 0xFFFF, "file {i} too large for the table");
        img[e + 4..e + 6].copy_from_slice(&ci.to_le_bytes());
        img[e + 6..e + 8].copy_from_slice(&(words as u16).to_le_bytes());
        img[e + 8..e + 10].copy_from_slice(&ce.to_le_bytes());
        img[e + 10..e + 12].copy_from_slice(&(words as u16).to_le_bytes());
    }
    let mut syms: Vec<(String, usize)> = vec![("lLBRelocTableAddr".into(), 0)];
    let mut add = |img: &mut Vec<u8>, name: &str, data: &[u8]| -> (usize, usize) {
        while img.len() % 16 != 0 { img.push(0); }
        let s = img.len();
        img.extend_from_slice(data);
        syms.push((format!("{name}_ROM_START"), s));
        syms.push((format!("{name}_ROM_END"), img.len()));
        (s, img.len())
    };
    let mut bankpos: HashMap<&str, (usize, usize)> = HashMap::new();
    for (name, a, b) in BANKS {
        let body = compile_bank(name, &rom[*a..*b]);
        let symname = if name.ends_with("_txb") { name.to_string() } else { format!("{name}_scb") };
        let p = add(&mut img, &symname, &body);
        bankpos.insert(name, p);
    }
    // music: a seqfile header, then sequences with their track tables swapped
    let (sa, sb) = (0xB277B0usize, 0xB4E5C0usize);
    let mut sbk = rom[sa..sb].to_vec();
    let count = be16(&sbk, 2);
    for i in 0..count {
        let e = 4 + i * 8;
        let (off, len) = (be32(&sbk, e), be32(&sbk, e + 4));
        if len >= 68 { for k in 0..17 { swap32(&mut sbk, off + k * 4); } }
        swap32(&mut sbk, e); swap32(&mut sbk, e + 4);
    }
    swap16(&mut sbk, 0); swap16(&mut sbk, 2);
    add(&mut img, "S1_music_sbk", &sbk);
    let mut c1 = rom[0xB4E5C0..B1_S1_CTL_END].to_vec();
    swap_ctl(&mut c1);
    add(&mut img, "B1_sounds1_ctl", &c1);
    add(&mut img, "B1_sounds1_tbl", &fs::read("assets/us/B1_sounds1.tbl.bin").unwrap());
    let mut c2 = rom[0xC6B650..0xC7B1F0].to_vec();
    swap_ctl(&mut c2);
    add(&mut img, "B1_sounds2_ctl", &c2);
    add(&mut img, "B1_sounds2_tbl", &fs::read("assets/us/B1_sounds2.tbl.bin").unwrap());
    let mut unk = fs::read("assets/us/fgm.unk.bin").unwrap();
    let c = be32(&unk, 0);
    for i in 0..c { for k in 0..3 { swap32(&mut unk, 4 + i * 16 + 4 + k * 4); } }
    swap32(&mut unk, 0);
    add(&mut img, "fgm_unk", &unk);
    for (f, name) in [("assets/us/fgm.tbl.bin", "fgm_tbl"), ("assets/us/fgm.ucd.bin", "fgm_ucd")] {
        let mut d = fs::read(f).unwrap();
        let c = be32(&d, 0);
        for i in 0..=c { swap32(&mut d, i * 4); }
        add(&mut img, name, &d);
    }
    // The resampler's filter table sits in the audio microcode's data.
    let lut = (0x400B0usize, 0x400B0usize + 512);
    assert_eq!(&rom[lut.0..lut.0 + 4], &[0x0c, 0x39, 0x66, 0xad], "resample table moved");
    let (lut_at, _) = add(&mut img, "rsp_resample_lut", &rom[lut.0..lut.1]);
    drop(add);
    fs::write("build/us/port/ssb64.data", &img).unwrap();

    // Names the N64 build got from the linker. wasm has no absolute symbols, so each
    // is a macro whose address is the value; `&name` then compiles unchanged.
    let mut h = String::from("// Generated by packdata. Do not edit.\n#ifndef PORT_SYMS_H\n#define PORT_SYMS_H\n");
    let mac = |h: &mut String, n: &str, v: usize| h.push_str(&format!("#define {n} (*(char *){v:#x}u)\n"));
    let rd = fs::read_to_string("include/reloc_data.us.h").unwrap();
    for l in rd.lines() {
        let Some(rest) = l.strip_prefix("extern int ") else { continue };
        let Some((name, val)) = rest.split_once("; // ") else { continue };
        let v = if let Some(x) = val.strip_prefix("0x") { usize::from_str_radix(x, 16).unwrap() } else { val.parse().unwrap() };
        mac(&mut h, name, v);
    }
    mac(&mut h, "lLBRelocTableFilesNum", n);
    for (name, v) in &syms { mac(&mut h, name, *v); }
    // particle bank aliases from symbols/linker_constants.txt
    for (alias, bank) in [("lEFCommon", "efcommon"), ("lGRPupupu", "grpupupu"), ("lGRYoster", "gryoster"), ("lGRHyrule", "grhyrule"), ("lMNTitle", "mntitle"), ("lITManager", "itcommon")] {
        let (a, b) = bankpos[bank];
        let (ta, tb) = bankpos[format!("{bank}_txb").as_str()];
        mac(&mut h, &format!("{alias}ParticleScriptBankLo"), a);
        mac(&mut h, &format!("{alias}ParticleScriptBankHi"), b);
        mac(&mut h, &format!("{alias}ParticleTextureBankLo"), ta);
        mac(&mut h, &format!("{alias}ParticleTextureBankHi"), tb);
    }
    // overlays do not exist in a static link
    let ov = fs::read_to_string("src/sc/scoverlay.h").unwrap();
    for l in ov.lines() {
        if let Some(rest) = l.strip_prefix("extern uintptr_t ") {
            let name = rest.trim_end_matches(';').trim();
            if name.ends_with("_BSS_END") && name.starts_with("ovl") {
                h.push_str(&format!("#define {name} (*(char *)gPortArena)\n"));
            } else { mac(&mut h, name, 0); }
        }
    }
    for x in ["scmanager_ROM_START","scmanager_ROM_END","scmanager_VRAM","scmanager_TEXT_START","scmanager_TEXT_END","scmanager_DATA_START","scmanager_RODATA_END","scmanager_BSS_START","scmanager_BSS_END","D_NF_00006010","D_NF_00006450"] { mac(&mut h, x, 0); }
    h.push_str(&format!("#define PORT_RESAMPLE_LUT_ROM {lut_at:#x}u\n"));
    h.push_str("#define dSYAudioPublicSettings2 dSYAudioPublicSettings\n#define dSYAudioPublicSettings3 dSYAudioPublicSettings\n");
    h.push_str("#endif\n");
    fs::create_dir_all("build/us/port/gen").unwrap();
    fs::write("build/us/port/gen/port_syms.h", h).unwrap();
    println!("image bytes {}", img.len());
}

/// File ids of fighter animations whose motion table entry sets FTANIM_FLAG_ANIMJOINT (0x8).
fn animjoint_file_ids() -> std::collections::HashSet<usize> {
    let mut ids = HashMap::new();
    for l in fs::read_to_string("include/reloc_data.us.h").unwrap().lines() {
        if let Some(rest) = l.strip_prefix("extern int ll") {
            if let Some((name, val)) = rest.split_once("FileID; // ") {
                if let Some(x) = val.strip_prefix("0x") { ids.insert(name.to_string(), usize::from_str_radix(x, 16).unwrap()); }
            }
        }
    }
    let mut out = std::collections::HashSet::new();
    let mut files = vec!["src/ft/ftdata.c".to_string()];
    for e in fs::read_dir("src/sc/scsubsys").unwrap() { files.push(e.unwrap().path().to_string_lossy().into_owned()); }
    for f in files {
        for l in fs::read_to_string(&f).unwrap_or_default().lines() {
            let Some(a) = l.find("&ll") else { continue };
            let Some(b) = l[a..].find("FileID") else { continue };
            let name = &l[a + 3..a + b];
            let flagged = l.contains("FTANIM_FLAG_ANIMJOINT") || l.trim_end().trim_end_matches(|c: char| c == '}' || c == ',' || c == ' ')
                .rsplit(',').next().map(|t| {
                    let t = t.trim();
                    t.strip_prefix("0x").and_then(|x| u32::from_str_radix(x, 16).ok()).map(|v| v & 8 != 0).unwrap_or(false)
                }).unwrap_or(false);
            if flagged { if let Some(id) = ids.get(name) { out.insert(*id); } }
        }
    }
    out
}

fn idents(text: &str) -> Vec<&str> {
    text.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).filter(|t| t.len() > 2 && t.starts_with('d') && t.as_bytes()[1].is_ascii_uppercase()).collect()
}

/// Names of blocks loaded by gsDPSetTextureImage or listed in a palette or image pointer array.
fn rdp_block_names<'a>(masters: impl Iterator<Item = &'a str>) -> std::collections::HashSet<String> {
    let mut out = std::collections::HashSet::new();
    let mut scan = |text: &str| {
        let mut rest = text;
        while let Some(i) = rest.find("gsDPSetTextureImage(") {
            rest = &rest[i + 20..];
            let end = rest.find(')').unwrap_or(rest.len());
            // the image is the last argument; a cast or offset may wrap it
            let mut e2 = end;
            if let Some(nl) = rest.find('\n') { e2 = nl.min(rest.len()); }
            if let Some(a) = rest[..e2].rfind("1,") { for t in idents(&rest[a..e2]) { out.insert(t.to_string()); } }
        }
        // a sprite's palette, and the stock icon palette tables: loaded with gDPLoadTLUT, read by the RDP
        for cast in ["(int*)", "(int *)"] {
            let mut rest = text;
            while let Some(i) = rest.find(cast) {
                rest = &rest[i + cast.len()..];
            let end = rest.find(|c: char| c == ',' || c == '\n').unwrap_or(rest.len());
            for t in idents(&rest[..end]) { out.insert(t.to_string()); }
            }
        }
        // Display lists expanded as raw words: `{ { 0xFDxxxxxx, (u32)dSym } }` is gsDPSetTextureImage too,
        // with the image (or TLUT) symbol as the second word, sometimes as `(u32)&dSym[off]`. Metal Mario's
        // chrome LUT and Meta Crystal's crystal maps are typed u16 and were only named this way, so the
        // image halfword-swapped them and the crystals came out as flat false-colour facets.
        let mut rest = text;
        while let Some(i) = rest.find("{ { 0xFD") {
            rest = &rest[i + 8..];
            let end = rest.find('\n').unwrap_or(rest.len());
            if let Some(c) = rest[..end].find("(u32)") { for t in idents(&rest[c..end]) { out.insert(t.to_string()); } }
        }
        // the same tables written as u32 words in some fighters' files
        let mut rest = text;
        while let Some(i) = rest.find("(u32)d") {
            rest = &rest[i + 5..];
            let end = rest.find(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).unwrap_or(rest.len());
            if rest[..end].contains("_lut") { out.insert(rest[..end].to_string()); }
        }
        // Material animations name their palettes (and texture frames) in `void *` tables (MObjSub
        // palette lists, e.g. Final Destination's platform seams): gcDrawMObj hands them to gDPLoadTLUT /
        // gDPSetTextureImage, so the RDP reads them. `void *` tables also hold sub-structures the CPU
        // reads, so only names declared in this file as palette or texture blocks are taken.
        let mut rest = text;
        while let Some(i) = rest.find("void *d") {
            rest = &rest[i + 7..];
            let Some(open) = rest.find('{') else { break };
            let semi = rest.find(';').unwrap_or(rest.len());
            if open > semi { continue; }
            let Some(close) = rest[open..].find('}') else { break };
            for t in idents(&rest[open..open + close]) {
                // the definition: `u16 dSym[N] = {` then its #include on the next line
                let key = format!(" {t}[");
                for (d, _) in text.match_indices(&key) {
                    let mut e = text.len().min(d + key.len() + 200); while !text.is_char_boundary(e) { e -= 1; }
                    let tail = &text[d + key.len()..e];
                    let Some(eq) = tail.find(']') else { continue };
                    if !tail[eq..].starts_with("] = {") { continue; }
                    let line = tail[eq..].lines().nth(1).unwrap_or("");
                    if line.contains("#include") && (line.contains(".palette.inc.c") || line.contains(".tex.inc.c")) { if std::env::var_os("RDPNAMES").is_some() { eprintln!("matanim table: {t}"); } out.insert(t.to_string()); }
                }
            }
        }
        for prefix in ["u16 *d", "u8 *d", "u16 * d", "u8 * d"] {
            let mut rest = text;
            while let Some(i) = rest.find(prefix) {
                rest = &rest[i + prefix.len()..];
                let Some(open) = rest.find('{') else { break };
                let semi = rest.find(';').unwrap_or(rest.len());
                if open > semi { continue; }
                let Some(close) = rest[open..].find('}') else { break };
                for t in idents(&rest[open..open + close]) { out.insert(t.to_string()); }
            }
        }
    };
    for m in masters { scan(&fs::read_to_string(m).unwrap_or_default()); }
    fn walk(dir: &str, f: &mut dyn FnMut(&str)) {
        let Ok(rd) = fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() { walk(&p.to_string_lossy(), f); }
            else if p.to_string_lossy().ends_with(".dl.inc.c") { f(&fs::read_to_string(&p).unwrap_or_default()); }
        }
    }
    walk("build/us/src/relocData", &mut |t| scan(t));
    out
}
