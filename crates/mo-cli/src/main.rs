//! The `mo` command. A placeholder until Phase 2 builds the real toolchain.

fn version_line() -> String {
    format!("mo {}", env!("CARGO_PKG_VERSION"))
}

fn main() {
    let first = std::env::args().nth(1);
    match first.as_deref() {
        Some("--version" | "-V") => println!("{}", version_line()),
        _ => {
            println!("{} (pre-release)", version_line());
            println!("Mo is in its design phase; there is nothing to run yet.");
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn version_line_names_the_command() {
        assert!(super::version_line().starts_with("mo "));
    }
}
