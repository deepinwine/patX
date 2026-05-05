//! 任务调度模块
//! 
//! 无锁并发任务调度

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};
use crossbeam::queue::SegQueue;

/// 导入格式
#[derive(Debug, Clone, Copy)]
pub enum ImportFormat {
    Excel,
    Csv,
    Json,
}

/// 导出格式
#[derive(Debug, Clone, Copy)]
pub enum ExportFormat {
    Excel,
    Csv,
    Json,
}

/// 导出过滤条件
#[derive(Debug, Clone, Default)]
pub struct ExportFilter {
    pub applicant: Option<String>,
    pub patent_type: Option<String>,
    pub date_from: Option<String>,
    pub date_to: Option<String>,
}

/// 任务类型
#[derive(Debug, Clone)]
pub enum TaskType {
    Import { file_path: String, format: ImportFormat },
    Export { file_path: String, format: ExportFormat, filter: Option<ExportFilter> },
    Search { query: String, limit: usize },
    BatchUpdate { ids: Vec<i32>, updates: std::collections::HashMap<String, String> },
    ParsePdf { file_paths: Vec<String> },
    Migration { source_db: String, target_db: String },
    Statistics,
}

/// 任务优先级
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum TaskPriority {
    Low = 0,
    Normal = 1,
    High = 2,
    Urgent = 3,
}

/// 任务状态
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TaskStatus {
    Pending,
    Running,
    Completed,
    Failed,
}

/// 任务结果
#[derive(Debug, Clone)]
pub struct TaskResult {
    pub success: bool,
    pub items_processed: usize,
    pub items_failed: usize,
    pub duration_ms: u64,
    pub message: String,
    pub data: Option<String>,
}

impl Default for TaskResult {
    fn default() -> Self {
        Self {
            success: true,
            items_processed: 0,
            items_failed: 0,
            duration_ms: 0,
            message: String::new(),
            data: None,
        }
    }
}

/// 任务
#[derive(Debug, Clone)]
pub struct Task {
    pub id: u64,
    pub task_type: TaskType,
    pub priority: TaskPriority,
    pub status: TaskStatus,
    pub retry_count: u32,
    pub created_at: Instant,
    pub started_at: Option<Instant>,
}

impl Task {
    pub fn new(task_type: TaskType) -> Self {
        static COUNTER: AtomicUsize = AtomicUsize::new(1);
        Self {
            id: COUNTER.fetch_add(1, Ordering::Relaxed) as u64,
            task_type,
            priority: TaskPriority::Normal,
            status: TaskStatus::Pending,
            retry_count: 0,
            created_at: Instant::now(),
            started_at: None,
        }
    }

    pub fn with_priority(mut self, priority: TaskPriority) -> Self {
        self.priority = priority;
        self
    }

    pub fn priority_score(&self) -> u8 {
        self.priority as u8
    }
}

/// 调度器统计
#[derive(Debug, Default)]
pub struct SchedulerStats {
    pub total_tasks: AtomicUsize,
    pub pending_tasks: AtomicUsize,
    pub running_tasks: AtomicUsize,
    pub completed_tasks: AtomicUsize,
    pub failed_tasks: AtomicUsize,
}

/// 线程池配置
#[derive(Debug, Clone)]
pub struct ThreadPoolConfig {
    pub num_threads: usize,
    pub queue_capacity: usize,
    pub thread_name: String,
}

impl Default for ThreadPoolConfig {
    fn default() -> Self {
        Self {
            num_threads: 4,
            queue_capacity: 1024,
            thread_name: "patx-worker".to_string(),
        }
    }
}

/// 任务调度器
pub struct TaskScheduler {
    config: ThreadPoolConfig,
    task_queue: Arc<SegQueue<Task>>,
    running: Arc<AtomicBool>,
    stats: Arc<SchedulerStats>,
    workers: Vec<thread::JoinHandle<()>>,
}

impl TaskScheduler {
    pub fn new(config: ThreadPoolConfig) -> Self {
        Self {
            config,
            task_queue: Arc::new(SegQueue::new()),
            running: Arc::new(AtomicBool::new(false)),
            stats: Arc::new(SchedulerStats::default()),
            workers: Vec::new(),
        }
    }

    pub fn start(&mut self) {
        if self.running.load(Ordering::SeqCst) {
            return;
        }
        
        self.running.store(true, Ordering::SeqCst);
        
        let num_threads = self.config.num_threads;
        for _ in 0..num_threads {
            let task_queue = Arc::clone(&self.task_queue);
            let running = Arc::clone(&self.running);
            
            let handle = thread::spawn(move || {
                Self::worker_loop(task_queue, running);
            });
            
            self.workers.push(handle);
        }
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::SeqCst);
    }

    pub fn submit(&self, task: Task) -> Result<u64, String> {
        if !self.running.load(Ordering::SeqCst) {
            return Err("Scheduler not running".to_string());
        }
        
        let task_id = task.id;
        self.task_queue.push(task);
        
        self.stats.total_tasks.fetch_add(1, Ordering::Relaxed);
        self.stats.pending_tasks.fetch_add(1, Ordering::Relaxed);
        
        Ok(task_id)
    }

    pub fn stats(&self) -> &SchedulerStats {
        &self.stats
    }

    pub fn queue_len(&self) -> usize {
        self.task_queue.len()
    }

    fn worker_loop(
        task_queue: Arc<SegQueue<Task>>,
        running: Arc<AtomicBool>,
    ) {
        while running.load(Ordering::Relaxed) {
            if let Some(_task) = task_queue.pop() {
                // 处理任务 (简化版)
                thread::sleep(Duration::from_millis(1));
            } else {
                thread::sleep(Duration::from_millis(10));
            }
        }
    }
}

impl Drop for TaskScheduler {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_task_creation() {
        let task = Task::new(TaskType::Statistics);
        assert_eq!(task.status, TaskStatus::Pending);
        assert_eq!(task.priority, TaskPriority::Normal);
    }

    #[test]
    fn test_task_priority() {
        let low = Task::new(TaskType::Statistics)
            .with_priority(TaskPriority::Low);
        let high = Task::new(TaskType::Statistics)
            .with_priority(TaskPriority::High);
        
        assert!(high.priority > low.priority);
    }
}
