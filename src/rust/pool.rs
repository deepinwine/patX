//! 内存池管理模块

use std::alloc::{alloc, dealloc, Layout};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicUsize, Ordering};

/// 对象池 - 预分配内存避免频繁申请释放
pub struct ObjectPool<T> {
    pool: Vec<NonNull<T>>,
    allocated: AtomicUsize,
    capacity: usize,
}

impl<T> ObjectPool<T> {
    pub fn new(capacity: usize) -> Self {
        let mut pool = Vec::with_capacity(capacity);
        
        for _ in 0..capacity {
            let layout = Layout::new::<T>();
            unsafe {
                let ptr = alloc(layout) as *mut T;
                if let Some(nn) = NonNull::new(ptr) {
                    pool.push(nn);
                }
            }
        }
        
        Self {
            pool,
            allocated: AtomicUsize::new(0),
            capacity,
        }
    }

    pub fn acquire(&mut self) -> Option<NonNull<T>> {
        if self.pool.is_empty() {
            None
        } else {
            self.allocated.fetch_add(1, Ordering::Relaxed);
            self.pool.pop()
        }
    }

    pub fn release(&mut self, ptr: NonNull<T>) {
        self.pool.push(ptr);
        self.allocated.fetch_sub(1, Ordering::Relaxed);
    }

    pub fn stats(&self) -> PoolStats {
        PoolStats {
            capacity: self.capacity,
            available: self.pool.len(),
            allocated: self.allocated.load(Ordering::Relaxed),
        }
    }
}

#[derive(Debug)]
pub struct PoolStats {
    pub capacity: usize,
    pub available: usize,
    pub allocated: usize,
}

// 全局专利内存池
lazy_static! {
    static ref PATENT_POOL: Mutex<ObjectPool<Patent>> = 
        Mutex::new(ObjectPool::new(10000));
}