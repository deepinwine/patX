//! 数据校验模块

use std::str;

/// 校验结果
#[derive(Debug)]
pub struct ValidationResult {
    pub is_valid: bool,
    pub errors: Vec<String>,
}

impl ValidationResult {
    pub fn new() -> Self {
        Self {
            is_valid: true,
            errors: Vec::new(),
        }
    }

    pub fn add_error(&mut self, error: &str) {
        self.is_valid = false;
        self.errors.push(error.to_string());
    }
}

/// 校验格科编码
pub fn validate_geke_code(code: &[u8]) -> bool {
    if code.is_empty() || code.len() > 16 {
        return false;
    }
    let s = match str::from_utf8(code) {
        Ok(s) => s.trim_end_matches('\0'),
        Err(_) => return false,
    };
    
    s.starts_with("GC-") || s.starts_with("GK-") || s.starts_with("PCT-")
}

/// 校验申请号
pub fn validate_application_no(no: &[u8]) -> bool {
    if no.is_empty() || no.len() > 32 {
        return false;
    }
    let s = match str::from_utf8(no) {
        Ok(s) => s.trim_end_matches('\0'),
        Err(_) => return false,
    };
    s.contains("CN") || s.starts_with("PCT/") || s.len() >= 10
}

/// 校验日期格式
pub fn validate_date(date: &[u8]) -> bool {
    if date.is_empty() {
        return true;
    }
    let s = match str::from_utf8(date) {
        Ok(s) => s.trim_end_matches('\0'),
        Err(_) => return false,
    };
    let parts: Vec<&str> = s.split('-').collect();
    if parts.len() != 3 {
        return false;
    }
    
    parts[0].len() == 4 
        && parts[1].len() == 2 
        && parts[2].len() == 2
}

/// 校验专利记录
pub fn validate_patent(geke_code: &[u8], app_no: &[u8], title: &[u8]) -> ValidationResult {
    let mut result = ValidationResult::new();
    
    if !validate_geke_code(geke_code) {
        result.add_error("Invalid geke code");
    }
    
    if !validate_application_no(app_no) {
        result.add_error("Invalid application number");
    }
    
    let title_str = str::from_utf8(title).unwrap_or("").trim_end_matches('\0');
    if title_str.is_empty() {
        result.add_error("Title cannot be empty");
    }
    
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_validate_geke_code() {
        assert!(validate_geke_code(b"GC-0001"));
        assert!(validate_geke_code(b"GK-0001"));
        assert!(!validate_geke_code(b"INVALID"));
    }

    #[test]
    fn test_validate_date() {
        assert!(validate_date(b"2024-01-15"));
        assert!(!validate_date(b"2024/01/15"));
    }
}