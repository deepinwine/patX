//! FFI 交互模块
//! 
//! C++ 与 Rust 交互接口

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

// C++ 函数声明
extern "C" {
    // 内存索引操作
    fn cpp_index_add(patent: *const Patent) -> i32;
    fn cpp_index_remove(id: i32) -> i32;
    fn cpp_index_find_by_geke(geke_code: *const c_char) -> *mut Patent;
    fn cpp_index_count() -> usize;
    
    // 数据库操作
    fn cpp_db_init(db_path: *const c_char) -> i32;
    fn cpp_db_close() -> i32;
    fn cpp_db_load_all() -> i32;
    fn cpp_db_save_all() -> i32;
}

/// 初始化系统
#[no_mangle]
pub extern "C" fn patx_init_system(db_path: *const c_char) -> i32 {
    unsafe {
        cpp_db_init(db_path)
    }
}

/// 加载所有数据
#[no_mangle]
pub extern "C" fn patx_load_data() -> i32 {
    unsafe {
        cpp_db_load_all()
    }
}

/// 获取专利数量
#[no_mangle]
pub extern "C" fn patx_get_count() -> usize {
    unsafe {
        cpp_index_count()
    }
}