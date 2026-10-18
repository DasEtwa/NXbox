use std::net::Ipv4Addr;

pub const PART_SIZE: u64 = 1024 * 1024 * 1024;
pub fn private_ipv4(host: &str) -> bool {
    host.parse::<Ipv4Addr>().is_ok_and(|ip| ip.is_private())
}
pub fn part_plan(name: &str, size: u64) -> Vec<(String, u64)> {
    if size <= PART_SIZE {
        return vec![(name.into(), size)];
    }
    (0..size.div_ceil(PART_SIZE))
        .map(|i| {
            (
                format!("{name}.nxpart{i:03}"),
                (size - i * PART_SIZE).min(PART_SIZE),
            )
        })
        .collect()
}
pub fn newest_package(packages: &[String]) -> Option<&str> {
    packages
        .iter()
        .filter(|p| p.starts_with("NSPX.NXbox_"))
        .max_by_key(|p| {
            p.split('_')
                .nth(1)
                .unwrap_or_default()
                .split('.')
                .map(|n| n.parse::<u64>().unwrap_or(0))
                .chain(std::iter::repeat(0))
                .take(4)
                .collect::<Vec<_>>()
        })
        .map(String::as_str)
}
pub fn upload_directory(name: &str) -> Result<&'static str, String> {
    if name.is_empty()
        || name.contains(['/', '\\', '"', '\r', '\n', '\0'])
        || name == "."
        || name == ".."
    {
        return Err("fileInvalid".into());
    }
    let lower = name.to_ascii_lowercase();
    if ["prod.keys", "title.keys"].contains(&lower.as_str()) {
        return Ok("LocalState/eden/keys");
    }
    match lower.rsplit('.').next() {
        Some("nsp" | "nsz" | "xci" | "xcz") => Ok("LocalState/games"),
        Some("nca") => Ok("LocalState/firmware"),
        _ => Err("fileInvalid".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn split_names_sizes_and_manifest() {
        let size = PART_SIZE * 5 / 2;
        assert_eq!(
            part_plan("Game.nsp", size),
            vec![
                ("Game.nsp.nxpart000".into(), PART_SIZE),
                ("Game.nsp.nxpart001".into(), PART_SIZE),
                ("Game.nsp.nxpart002".into(), PART_SIZE / 2)
            ]
        );
        assert_eq!(
            part_plan("small.nsp", PART_SIZE),
            vec![("small.nsp".into(), PART_SIZE)]
        );
    }
    #[test]
    fn certificate_hosts() {
        for host in ["192.168.0.5", "10.1.2.3", "172.20.0.1"] {
            assert!(private_ipv4(host));
        }
        for host in [
            "8.8.8.8",
            "172.32.0.1",
            "xbox.local",
            "localhost",
            "127.0.0.1",
            "192.168.0.5:443",
            "::1",
        ] {
            assert!(!private_ipv4(host));
        }
    }
    #[test]
    fn package_versions() {
        let packages = [
            "NSPX.NXbox_2.0.0.0_x64",
            "NSPX.NXbox_10.0.0.0_x64",
            "NSPX.NXboxGame_99.0.0.0_x64",
        ]
        .map(String::from);
        assert_eq!(newest_package(&packages), Some("NSPX.NXbox_10.0.0.0_x64"));
        assert_eq!(newest_package(&[]), None);
        assert_eq!(
            newest_package(&["NSPX.NXboxGame_99.0.0.0_x64".into()]),
            None
        );
    }
    #[test]
    fn upload_paths() {
        for name in ["Game.nsp", "Game.nsz", "Game.xci", "Game.xcz"] {
            assert_eq!(upload_directory(name).unwrap(), "LocalState/games");
        }
        for name in ["prod.keys", "title.keys"] {
            assert_eq!(upload_directory(name).unwrap(), "LocalState/eden/keys");
        }
        assert_eq!(upload_directory("abc.nca").unwrap(), "LocalState/firmware");
        assert!(upload_directory("../prod.keys").is_err());
    }
}
