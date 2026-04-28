//! 数据校验模块
//! 
//! 复刻 Python 版校验规则

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

/// 校验专利编号
pub fn validate_geke_code(code: &[u8]) -> bool {
    if code.is_empty() || code.len() > 16 {
        return false;
    }
    // 格式: GC-XXXX 或 GC-XXXX（项目）
    let s = match str::from_utf8(code) {
        Ok(s) => s.trim_end('\0'),
        Err(_) => return false,
    };
    
    s.starts_with("GC-") || s.contains("（")|| s.starts_with("PCT-")
}

/// 校验申请号
pub fn validate_application_no(no: &[u8]) -> bool {
    if no.is_empty() || no.len() > 32 {
        return false;
    }
    let s = match str::from_utf8(no) {
        Ok(s) => s.trim_end('\0'),
        Err(_) => return false,
    };
    // 中国专利申请号格式: CNXXXXXXXX.X
    // PCT格式: PCT/CNXXXX/XXXXX
    s.contains("CN") || s.starts_with("PCT/") || s.len() >= 10
}

/// 校验日期格式
pub fn validate_date(date: &[u8]) -> bool {
    if date.is_empty() {
        return true; // 空日期允许
    }
    let s = match str::from_utf8(date) {
        Ok(s) => s.trim_end('\0'),
        Err(_) => return false,
    };
    // 格式: YYYY-MM-DD
    let parts: Vec<&str> = s.split('-').collect();
    if parts.len() != 3 {
        return false;
    }
    
    parts[0].len() == 4 
        && parts[1].len() == 2 
        && parts[2].len() == 2
        && parts[0].parse::<u32>().is_ok()
        && parts[1].parse::<u32>().is_ok()
        && parts[2].parse::<u32>().is_ok()
}

/// 校验完整专利记录
pub fn validate_patent(patent: &Patent) -> ValidationResult {
    let mut result = ValidationResult::new();
    
    if !validate_geke_code(&patent.geke_code) {
        result.add_error("无效的格科编码");
    }
    
    if !validate_application_no(&patent.application_no) {
        result.add_error("无效的申请号");
    }
    
    if !validate_date(&patent.application_date) {
        result.add_error("无效的申请日期格式");
    }
    
    // 检查必填字段
    let title = str::from_utf8(&patent.title).unwrap_or("").trim_end('\0');
    if title.is_empty() {
        result.add_error("发明名称不能为空");
    }
    
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_validate_geke_code() {
        assert!(validate_geke_code(b"GC-0001"));
        assert!(validate_geke_code(b"GC-0001（临港）"));
        assert!(!validate_geke_code(b"INVALID"));
    }

    #[test]
    fn test_validate_date() {
        assert!(validate_date(b"2024-01-15"));
        assert!(!validate_date(b"2024/01/15"));
    }
}