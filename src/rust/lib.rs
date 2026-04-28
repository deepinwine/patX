//! patX Core - Rust 安全并发中间层

pub mod ffi;
pub mod pool;
pub mod validator;
pub mod scheduler;

use std::os::raw::c_char;

/// 专利数据结构 (与 C++ 共享)
#[repr(C)]
pub struct Patent {
    pub id: i32,
    pub geke_code: [u8; 16],
    pub application_no: [u8; 32],
    pub title: [u8; 256],
    pub applicant: [u8; 128],
    pub application_date: [u8; 16],
    pub patent_type: [u8; 32],
    pub legal_status: [u8; 32],
    pub inventor: [u8; 256],
    pub classification: [u8; 64],
    pub agent: [u8; 64],
    pub agency: [u8; 128],
    pub publish_no: [u8; 32],
    pub publish_date: [u8; 16],
    pub grant_date: [u8; 16],
    pub expire_date: [u8; 16],
    pub handler: [u8; 32],
    pub notes: [u8; 512],
    pub patent_level: i32,
    pub is_valid: i32,
    pub created_at: [u8; 32],
    pub updated_at: [u8; 32],
}

/// 系统初始化
#[no_mangle]
pub extern "C" fn patx_init() -> i32 {
    println!("[patX] System initialized");
    0
}

/// 系统关闭
#[no_mangle]
pub extern "C" fn patx_shutdown() -> i32 {
    println!("[patX] System shutdown");
    0
}

/// 获取版本
#[no_mangle]
pub extern "C" fn patx_version() -> *mut c_char {
    unsafe { 
        std::ffi::CString::new("0.1.0")
            .unwrap()
            .into_raw() 
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_init() {
        assert_eq!(patx_init(), 0);
    }
}