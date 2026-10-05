#ifndef APP_TASK_H_
#define APP_TASK_H_

// The bt_app task runs the BTstack run loop. BTstack is not thread-safe, so this task owns all
// application state; other tasks hand work to it through btstack_run_loop_execute_on_main_thread()
// (DESIGN.md §5).
void app_task_start();

#endif // APP_TASK_H_
