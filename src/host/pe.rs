//! The DLLs a Windows executable imports at load time, read from its PE import
//! table, so the host can tell whether the bundled console server can start on
//! this Windows before it uses the pair (D6). Pure and portable: it only reads
//! bytes.

/// At most this many import descriptors are read.
const MAX_IMPORTS: usize = 4096;
/// At most this many bytes of a DLL name are read.
const MAX_NAME: usize = 260;

/// The DLL names in the import directory of the PE32+ `image`, in table order;
/// None when `image` is not a PE32+ image or its import table cannot be read.
/// Delay-loaded DLLs are not listed: they are loaded on first use, if ever.
pub(crate) fn imports(image: &[u8]) -> Option<Vec<String>> {
    if image.get(..2)? != b"MZ" {
        return None;
    }
    let header = u32_at(image, 0x3C)? as usize;
    if image.get(header..header.checked_add(4)?)? != b"PE\0\0" {
        return None;
    }
    let coff = header + 4;
    let sections = u16_at(image, coff + 2)? as usize;
    let optional_size = u16_at(image, coff + 16)? as usize;
    let optional = coff + 20;
    // PE32+ only: the host is x64 and so is the bundled pair.
    if u16_at(image, optional)? != 0x20B || optional_size < 112 + 2 * 8 {
        return None;
    }
    if u32_at(image, optional + 108)? < 2 {
        return Some(Vec::new());
    }
    let directory = u32_at(image, optional + 112 + 8)?;
    if directory == 0 {
        return Some(Vec::new());
    }
    let table = optional + optional_size;
    let offset = |rva: u32| -> Option<usize> {
        (0..sections).find_map(|index| {
            let section = table + index * 40;
            let virtual_size = u32_at(image, section + 8)?;
            let address = u32_at(image, section + 12)?;
            let raw_size = u32_at(image, section + 16)?;
            let raw = u32_at(image, section + 20)?;
            let span = virtual_size.max(raw_size);
            (rva >= address && rva - address < span && rva - address < raw_size)
                .then(|| (raw + (rva - address)) as usize)
        })
    };
    let mut names = Vec::new();
    let mut descriptor = offset(directory)?;
    for _ in 0..MAX_IMPORTS {
        let entry = image.get(descriptor..descriptor.checked_add(20)?)?;
        if entry.iter().all(|byte| *byte == 0) {
            return Some(names);
        }
        let name = offset(u32_at(image, descriptor + 12)?)?;
        let bytes = image.get(name..)?;
        let end = bytes.iter().take(MAX_NAME).position(|byte| *byte == 0)?;
        let name = std::str::from_utf8(&bytes[..end]).ok()?;
        if name.is_empty() || !name.is_ascii() {
            return None;
        }
        names.push(name.to_owned());
        descriptor += 20;
    }
    None
}

/// Whether `name` is an API set (`api-ms-win-*`, `ext-ms-win-*`), which the
/// loader maps to whatever implements it rather than to a file of that name.
pub(crate) fn api_set(name: &str) -> bool {
    let name = name.to_ascii_lowercase();
    name.starts_with("api-ms-win-") || name.starts_with("ext-ms-win-")
}

fn u16_at(image: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        image.get(at..at.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn u32_at(image: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        image.get(at..at.checked_add(4)?)?.try_into().ok()?,
    ))
}

#[cfg(test)]
/// A minimal PE32+ image whose one section holds an import table naming
/// `dlls`, the section starting at RVA 0x1000 and file offset 0x200.
pub(crate) fn image_importing(dlls: &[&str]) -> Vec<u8> {
    let mut image = vec![0u8; 0x200];
    image[..2].copy_from_slice(b"MZ");
    image[0x3C..0x40].copy_from_slice(&0x80u32.to_le_bytes());
    image[0x80..0x84].copy_from_slice(b"PE\0\0");
    let coff = 0x84;
    image[coff..coff + 2].copy_from_slice(&0x8664u16.to_le_bytes());
    image[coff + 2..coff + 4].copy_from_slice(&1u16.to_le_bytes());
    image[coff + 16..coff + 18].copy_from_slice(&240u16.to_le_bytes());
    let optional = coff + 20;
    image[optional..optional + 2].copy_from_slice(&0x20Bu16.to_le_bytes());
    image[optional + 108..optional + 112].copy_from_slice(&16u32.to_le_bytes());
    image[optional + 120..optional + 124].copy_from_slice(&0x1000u32.to_le_bytes());
    // The section: descriptors first, then the names.
    let descriptors = (dlls.len() + 1) * 20;
    let mut section = vec![0u8; descriptors];
    for (index, dll) in dlls.iter().enumerate() {
        let rva = 0x1000 + section.len() as u32;
        section[index * 20 + 12..index * 20 + 16].copy_from_slice(&rva.to_le_bytes());
        section.extend_from_slice(dll.as_bytes());
        section.push(0);
    }
    let table = optional + 240;
    let size = section.len() as u32;
    image[table..table + 8].copy_from_slice(b".rdata\0\0");
    image[table + 8..table + 12].copy_from_slice(&size.to_le_bytes());
    image[table + 12..table + 16].copy_from_slice(&0x1000u32.to_le_bytes());
    image[table + 16..table + 20].copy_from_slice(&size.to_le_bytes());
    image[table + 20..table + 24].copy_from_slice(&0x200u32.to_le_bytes());
    image.extend_from_slice(&section);
    image
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_import_table_names_every_load_time_dll() {
        let dlls = [
            "api-ms-win-core-file-l1-1-0.dll",
            "D3DCOMPILER_47.dll",
            "USER32.dll",
        ];
        assert_eq!(imports(&image_importing(&dlls)).unwrap(), dlls);
        assert_eq!(
            imports(&image_importing(&[])).unwrap(),
            Vec::<String>::new()
        );
    }

    #[test]
    fn anything_but_a_readable_pe32_plus_image_is_none() {
        assert_eq!(imports(b""), None);
        assert_eq!(imports(b"MZ"), None);
        assert_eq!(imports(&[0u8; 0x400]), None);
        let full = image_importing(&["USER32.dll"]);
        // Cut inside the name, or a PE32 (not PE32+) optional header.
        assert_eq!(imports(&full[..full.len() - 4]), None);
        let mut pe32 = full.clone();
        pe32[0x98..0x9A].copy_from_slice(&0x10Bu16.to_le_bytes());
        assert_eq!(imports(&pe32), None);
        // A descriptor whose name points outside every section.
        let mut stray = full;
        let first = 0x200 + 12;
        stray[first..first + 4].copy_from_slice(&0x9000u32.to_le_bytes());
        assert_eq!(imports(&stray), None);
    }

    #[test]
    fn api_sets_are_not_files() {
        assert!(api_set("api-ms-win-core-com-l1-1-0.dll"));
        assert!(api_set("EXT-MS-WIN-uiacore-l1-1-0.dll"));
        assert!(!api_set("D3DCOMPILER_47.dll"));
        assert!(!api_set("ntdll.dll"));
    }
}
