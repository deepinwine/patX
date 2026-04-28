//! FFI 交互模块

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

/// C 字符串转 Rust String
pub fn c_str_to_string(s: *const c_char) -> String {
    if s.is_null() {
        return String::new();
    }
    unsafe {
        CStr::from_ptr(s)
            .to_string_lossy()
            .into_owned()
    }
}

/// Rust String 转 C 字符串
pub fn string_to_c_str(s: String) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// 释放 C 字符串内存
#[no_mangle]
pub extern "C" fn patx_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe {
            let _ = CString::from_raw(s);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_c_str_conversion() {
        let original = "test string";
        let c_str = string_to_c_str(original.to_string());
        assert!(!c_str.is_null());
        
        let rust_str = c_str_to_string(c_str);
        assert_eq!(rust_str, original);
        
        patx_free_string(c_str);
    }
}