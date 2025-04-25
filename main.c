#include <stdio.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>

#define DOWNLOADS_DIR "downloads"
#define MAX_PATH 1024
#define BURST_TIME 100000 // 100ms time for round robin
#define MAX_THREADS 5
#define LOG_FILE "download_manager.log"
#define BUFFER_SIZE 8192
#define MAX_CONCURRENT_DOWNLOADS MAX_THREADS

typedef struct
{
    char url[1024];
    char filename[256];
    char full_path[MAX_PATH];
    bool active;
    pthread_t thread;
    double progress;
    CURLcode result;
    bool paused;
    bool cancelled;
    pthread_cond_t pause_cond;
    CURL *curl_handle;
    FILE *file_handle;
    int priority;
    size_t speed;
    time_t last_update_time;
    double last_downloaded;
    time_t start_time;
    time_t end_time;
} DownloadTask;

typedef struct
{
    char *data;
    size_t size;
    DownloadTask *task;
} ChunkBuffer;

typedef struct
{
    ChunkBuffer *buffers;
    int capacity;
    int front;
    int rear;
    int count;
    pthread_mutex_t mutex;
    sem_t empty;
    sem_t full;
} BufferQueue;

typedef struct
{
    int active_tasks[MAX_CONCURRENT_DOWNLOADS];
    int count;
} ActiveTaskList;

ActiveTaskList active_downloads = {.count = 0};
DownloadTask *downloads = NULL;
int num_downloads = 0;
int current_task = -1;
pthread_mutex_t lock;
pthread_mutex_t scheduler_lock;
pthread_mutex_t log_mutex;
sem_t thread_pool_sem;
bool running = true;
BufferQueue chunk_queue;

void log_message(const char *message)
{
    time_t now;
    time(&now);
    char *time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';

    pthread_mutex_lock(&log_mutex);

    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file)
    {
        fprintf(log_file, "[%s] %s\n", time_str, message);
        fclose(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}

void enqueue_chunk(BufferQueue *queue, ChunkBuffer chunk);

int create_downloads_dir()
{
    struct stat st = {0};
    if (stat(DOWNLOADS_DIR, &st) == -1)
    {
        if (mkdir(DOWNLOADS_DIR, 0755) == -1)
        {
            perror("Error creating downloads directory");
            log_message("Error creating downloads directory");
            return -1;
        }
    }
    return 0;
}

void init_buffer_queue(BufferQueue *queue, int capacity)
{
    queue->buffers = malloc(capacity * sizeof(ChunkBuffer));
    queue->capacity = capacity;
    queue->front = 0;
    queue->rear = 0;
    queue->count = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    sem_init(&queue->empty, 0, capacity);
    sem_init(&queue->full, 0, 0);
}

int progress_func(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow)
{
    DownloadTask *task = (DownloadTask *)clientp;

    pthread_mutex_lock(&lock);
    if (task->cancelled)
    {
        pthread_mutex_unlock(&lock);
        return 1;
    }

    time_t now = time(NULL);

    if (task->start_time == 0)
    {
        task->start_time = now;
        task->last_update_time = now;
        task->last_downloaded = dlnow;
        task->speed = 0;
    }
    else
    {
        double elapsed = difftime(now, task->last_update_time);
        if (elapsed >= 0.1 && dlnow >= task->last_downloaded)
        {
            curl_off_t delta = dlnow - task->last_downloaded;
            task->speed = (size_t)((double)delta / elapsed);
            task->last_update_time = now;
            task->last_downloaded = dlnow;
        }
    }

    if (dltotal > 0)
    {
        task->progress = fmin(fmax((double)dlnow / dltotal, 0.0), 1.0);
    }
    else
    {
        task->progress = -1.0;
    }

    pthread_mutex_unlock(&lock);
    return 0;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream)
{
    DownloadTask *task = (DownloadTask *)stream;
    size_t real_size = size * nmemb;

    pthread_mutex_lock(&lock);
    bool cancelled = task->cancelled;
    pthread_mutex_unlock(&lock);

    if (cancelled)
    {
        return 0;
    }

    ChunkBuffer chunk;
    chunk.data = malloc(real_size);
    if (!chunk.data)
    {
        return 0;
    }

    memcpy(chunk.data, ptr, real_size);
    chunk.size = real_size;
    chunk.task = task;

    enqueue_chunk(&chunk_queue, chunk);

    return real_size;
}

void enqueue_chunk(BufferQueue *queue, ChunkBuffer chunk)
{
    sem_wait(&queue->empty);
    pthread_mutex_lock(&queue->mutex);

    queue->buffers[queue->rear] = chunk;
    queue->rear = (queue->rear + 1) % queue->capacity;
    queue->count++;

    pthread_mutex_unlock(&queue->mutex);
    sem_post(&queue->full);
}

ChunkBuffer dequeue_chunk(BufferQueue *queue)
{
    sem_wait(&queue->full);
    pthread_mutex_lock(&queue->mutex);

    ChunkBuffer chunk = queue->buffers[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->count--;

    pthread_mutex_unlock(&queue->mutex);
    sem_post(&queue->empty);
    return chunk;
}

void *file_writer_thread(void *arg)
{
    while (running)
    {
        ChunkBuffer chunk = dequeue_chunk(&chunk_queue);

        pthread_mutex_lock(&lock);
        if (chunk.task->file_handle && !chunk.task->cancelled)
        {
            size_t written = fwrite(chunk.data, 1, chunk.size, chunk.task->file_handle);
            if (written != chunk.size)
            {
                perror("Error writing to file");
            }
            fflush(chunk.task->file_handle);
        }
        pthread_mutex_unlock(&lock);

        free(chunk.data);
    }
    return NULL;
}

void pause_download(DownloadTask *task)
{
    pthread_mutex_lock(&lock);
    if (!task->paused && task->active && !task->cancelled)
    {
        task->paused = true;
        if (task->curl_handle)
        {
            curl_easy_pause(task->curl_handle, CURLPAUSE_RECV);
        }
        // if active, set unactive
        for (int i = 0; i < active_downloads.count;)
        {
            if (active_downloads.active_tasks[i] == (task - downloads))
            {
                memmove(&active_downloads.active_tasks[i],
                        &active_downloads.active_tasks[i + 1],
                        (active_downloads.count - i - 1) * sizeof(int));
                active_downloads.count--;
            }
            else
            {
                i++;
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

void resume_download(DownloadTask *task)
{
    pthread_mutex_lock(&lock);
    if (task->paused && task->active && !task->cancelled)
    {
        task->paused = false;
        if (task->curl_handle)
        {
            curl_easy_pause(task->curl_handle, CURLPAUSE_CONT);
        }
        pthread_cond_signal(&task->pause_cond);
    }
    pthread_mutex_unlock(&lock);
}

void cancel_download(DownloadTask *task)
{
    pthread_mutex_lock(&lock);
    if (!task->cancelled && task->active)
    {
        task->cancelled = true;
        task->active = false;

        // if active, set unactive
        for (int i = 0; i < active_downloads.count;)
        {
            if (active_downloads.active_tasks[i] == (task - downloads))
            {
                memmove(&active_downloads.active_tasks[i],
                        &active_downloads.active_tasks[i + 1],
                        (active_downloads.count - i - 1) * sizeof(int));
                active_downloads.count--;
            }
            else
            {
                i++;
            }
        }

        if (task->curl_handle)
        {
            curl_easy_pause(task->curl_handle, CURLPAUSE_RECV);
        }

        pthread_cond_signal(&task->pause_cond);

        if (task->file_handle)
        {
            fclose(task->file_handle);
            task->file_handle = NULL;
        }

        remove(task->full_path);
    }
    pthread_mutex_unlock(&lock);
}

void *scheduler_thread(void *arg)
{
    while (running)
    {
        pthread_mutex_lock(&scheduler_lock);

        if (num_downloads > 0)
        {
            for (int i = 0; i < active_downloads.count;)
            {
                int task_idx = active_downloads.active_tasks[i];
                if (task_idx >= num_downloads ||
                    !downloads[task_idx].active ||
                    downloads[task_idx].cancelled)
                {
                    // Remove from active list
                    memmove(&active_downloads.active_tasks[i],
                            &active_downloads.active_tasks[i + 1],
                            (active_downloads.count - i - 1) * sizeof(int));
                    active_downloads.count--;
                }
                else
                {
                    i++;
                }
            }

            while (active_downloads.count < MAX_CONCURRENT_DOWNLOADS)
            {
                int next_task = -1;
                int start_index = 0;

                if (active_downloads.count > 0)
                {
                    start_index = (active_downloads.active_tasks[active_downloads.count - 1] + 1) % num_downloads;
                }

                for (int i = 0; i < num_downloads; i++)
                {
                    int candidate = (start_index + i) % num_downloads;
                    DownloadTask *task = &downloads[candidate];

                    if (task->active && !task->cancelled && !task->paused)
                    {
                        int already_active = 0;
                        for (int j = 0; j < active_downloads.count; j++)
                        {
                            if (active_downloads.active_tasks[j] == candidate)
                            {
                                already_active = 1;
                                break;
                            }
                        }

                        if (!already_active)
                        {
                            next_task = candidate;
                            break;
                        }
                    }
                }

                if (next_task == -1)
                    break;

                active_downloads.active_tasks[active_downloads.count++] = next_task;
                resume_download(&downloads[next_task]);
            }
        }

        pthread_mutex_unlock(&scheduler_lock);
        usleep(BURST_TIME);
    }
    return NULL;
}

void *download_thread(void *arg)
{
    DownloadTask *task = (DownloadTask *)arg;
    int retry_count = 0;
    const int max_retries = 3;

    sem_wait(&thread_pool_sem);

    task->start_time = time(NULL);
    task->last_update_time = task->start_time;
    task->last_downloaded = 0;
    task->speed = 0;
    char log_msg[2048];
    snprintf(log_msg, sizeof(log_msg), "Download started: %s", task->url);
    log_message(log_msg);

    while (retry_count < max_retries)
    {
        pthread_mutex_lock(&lock);
        if (task->cancelled)
        {
            pthread_mutex_unlock(&lock);
            break;
        }
        pthread_mutex_unlock(&lock);

        task->curl_handle = curl_easy_init();
        if (task->curl_handle)
        {
            int fd = open(task->full_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd == -1)
            {
                fprintf(stderr, "Failed to open file: %s\n", task->full_path);
                task->result = CURLE_WRITE_ERROR;
                break;
            }

            ftruncate(fd, 0);
            task->file_handle = fdopen(fd, "wb");

            if (!task->file_handle)
            {
                fprintf(stderr, "Failed to open file: %s\n", task->full_path);
                task->result = CURLE_WRITE_ERROR;
                close(fd);
                break;
            }

            curl_easy_setopt(task->curl_handle, CURLOPT_URL, task->url);
            curl_easy_setopt(task->curl_handle, CURLOPT_WRITEFUNCTION, write_data);
            curl_easy_setopt(task->curl_handle, CURLOPT_WRITEDATA, task);
            curl_easy_setopt(task->curl_handle, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(task->curl_handle, CURLOPT_XFERINFODATA, task);
            curl_easy_setopt(task->curl_handle, CURLOPT_XFERINFOFUNCTION, progress_func);
            curl_easy_setopt(task->curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1024);
            curl_easy_setopt(task->curl_handle, CURLOPT_LOW_SPEED_TIME, 30);

            curl_easy_pause(task->curl_handle, CURLPAUSE_RECV);

            pthread_mutex_lock(&lock);
            while (task->paused && !task->cancelled)
            {
                pthread_cond_wait(&task->pause_cond, &lock);
            }

            if (task->cancelled)
            {
                pthread_mutex_unlock(&lock);
                curl_easy_cleanup(task->curl_handle);
                task->curl_handle = NULL;
                break;
            }
            pthread_mutex_unlock(&lock);

            task->result = curl_easy_perform(task->curl_handle);

            if (task->result == CURLE_ABORTED_BY_CALLBACK)
            {
                pthread_mutex_lock(&lock);
                while (task->paused && !task->cancelled)
                {
                    pthread_cond_wait(&task->pause_cond, &lock);
                }

                if (task->cancelled)
                {
                    pthread_mutex_unlock(&lock);
                    break;
                }
                pthread_mutex_unlock(&lock);

                task->result = CURLE_OK;
                continue;
            }

            if (task->result == CURLE_OK)
            {
                break;
            }
            else
            {
                retry_count++;
                if (retry_count < max_retries)
                {
                    snprintf(log_msg, sizeof(log_msg),
                             "Download failed (attempt %d/%d): %s - %s",
                             retry_count, max_retries, task->url,
                             curl_easy_strerror(task->result));
                    log_message(log_msg);

                    fseek(task->file_handle, 0, SEEK_SET);
                    ftruncate(fileno(task->file_handle), 0);
                }
            }

            curl_easy_cleanup(task->curl_handle);
            task->curl_handle = NULL;
        }
    }

    pthread_mutex_lock(&lock);
    if (task->file_handle)
    {
        fclose(task->file_handle);
        task->file_handle = NULL;
    }

    if (task->cancelled)
    {
        remove(task->full_path);
        snprintf(log_msg, sizeof(log_msg), "Download cancelled: %s", task->url);
    }
    else if (task->result == CURLE_OK)
    {
        task->end_time = time(NULL);
        snprintf(log_msg, sizeof(log_msg), "Download completed: %s (%.2f KB/s)",
                 task->url,
                 (task->progress > 0 ? (task->speed / 1024.0) : 0));
    }
    else
    {
        remove(task->full_path);
        snprintf(log_msg, sizeof(log_msg), "Download failed after %d attempts: %s",
                 max_retries, task->url);
    }

    task->active = false;
    log_message(log_msg);
    pthread_mutex_unlock(&lock);

    sem_post(&thread_pool_sem);
    return NULL;
}

void add_download(const char *url, const char *custom_filename, int priority)
{
    pthread_mutex_lock(&lock);

    downloads = realloc(downloads, (num_downloads + 1) * sizeof(DownloadTask));
    if (!downloads)
    {
        perror("Memory allocation failed");
        pthread_mutex_unlock(&lock);
        return;
    }

    DownloadTask *task = &downloads[num_downloads];
    memset(task, 0, sizeof(DownloadTask));

    strncpy(task->url, url, sizeof(task->url) - 1);

    if (custom_filename && custom_filename[0])
    {
        strncpy(task->filename, custom_filename, sizeof(task->filename) - 1);
    }
    else
    {
        const char *last_slash = strrchr(url, '/');
        if (last_slash && last_slash[1])
        {
            strncpy(task->filename, last_slash + 1, sizeof(task->filename) - 1);
        }
        else
        {
            snprintf(task->filename, sizeof(task->filename), "download_%d", num_downloads + 1);
        }
    }

    snprintf(task->full_path, sizeof(task->full_path), "%s/%s", DOWNLOADS_DIR, task->filename);

    task->progress = 0.0;
    task->active = true;
    task->paused = false;
    task->cancelled = false;
    task->result = CURLE_OK;
    task->priority = priority;
    task->speed = 0;
    pthread_cond_init(&task->pause_cond, NULL);

    if (pthread_create(&task->thread, NULL, download_thread, task) != 0)
    {
        perror("Failed to create thread");
        task->active = false;
        pthread_cond_destroy(&task->pause_cond);
    }
    else
    {
        num_downloads++;
        char log_msg[4096];
        snprintf(log_msg, sizeof(log_msg), "Added download: %s -> %s (Priority: %d)",
                 task->url, task->full_path, task->priority);
        log_message(log_msg);
    }

    pthread_mutex_unlock(&lock);
}

void view_downloads()
{
    pthread_mutex_lock(&lock);

    printf("\nCurrent Downloads:\n");
    printf("----------------------------------------------------------------------------\n");
    for (int i = 0; i < num_downloads; i++)
    {
        DownloadTask *task = &downloads[i];
        printf("%d. %s\n", i + 1, task->url);
        printf("   Saving to: %s\n", task->full_path);
        printf("   Priority: %d\n", task->priority);

        if (task->progress >= 0.0 && task->progress <= 1.0)
        {
            int bar_width = 30;
            int filled = (int)(task->progress * bar_width);
            printf("   Progress: [");
            for (int j = 0; j < bar_width; j++)
            {
                printf(j < filled ? "#" : "-");
            }
            printf("] %.0f%%", task->progress * 100);
        }
        else
        {
            printf("   Progress: Calculating...");
        }

        if (task->speed > 0)
        {
            printf(" (%.2f KB/s)", task->speed / 1024.0);
        }

        if (task->active)
        {
            printf(" %s\n", task->paused ? "(Paused)" : "(Downloading)");
        }
        else if (task->cancelled)
        {
            printf(" (Cancelled)\n");
        }
        else if (task->result == CURLE_OK)
        {
            printf(" (Completed successfully)\n");
        }
        else
        {
            printf(" (Failed: %s)\n", curl_easy_strerror(task->result));
        }

        printf("\n");
    }
    printf("----------------------------------------------------------------------------\n");

    pthread_mutex_unlock(&lock);
}

void cleanup_downloads()
{
    pthread_mutex_lock(&lock);

    int new_count = 0;
    for (int i = 0; i < num_downloads; i++)
    {
        if (downloads[i].active)
        {
            if (i != new_count)
            {
                memcpy(&downloads[new_count], &downloads[i], sizeof(DownloadTask));
            }
            new_count++;
        }
        else
        {
            if (downloads[i].thread)
            {
                pthread_join(downloads[i].thread, NULL);
            }
            pthread_cond_destroy(&downloads[i].pause_cond);

            if (downloads[i].curl_handle)
            {
                curl_easy_cleanup(downloads[i].curl_handle);
                downloads[i].curl_handle = NULL;
            }
        }
    }

    num_downloads = new_count;
    downloads = realloc(downloads, num_downloads * sizeof(DownloadTask));

    if (current_task >= num_downloads)
    {
        current_task = -1;
    }

    pthread_mutex_unlock(&lock);
}

void print_menu()
{
    printf("\nMENU\n");
    printf("1: View current downloads\n");
    printf("2: Add new download\n");
    printf("3: Pause a download\n");
    printf("4: Resume a download\n");
    printf("5: Cancel a download\n");
    printf("6: View download logs\n");
    printf("7: Exit\n");
    printf("Enter option: ");
}

void view_logs()
{
    FILE *log_file = fopen(LOG_FILE, "r");
    if (!log_file)
    {
        printf("No logs available yet.\n");
        return;
    }

    printf("\nDownload Logs:\n");
    printf("----------------------------------------------------------------------------\n");

    char line[2048];
    while (fgets(line, sizeof(line), log_file))
    {
        printf("%s", line);
    }

    printf("----------------------------------------------------------------------------\n");
    fclose(log_file);
}
 
    
    
int main()
{
    if (create_downloads_dir() != 0)
    {
        return 1;
    }

    if (pthread_mutex_init(&lock, NULL) != 0 ||
        pthread_mutex_init(&scheduler_lock, NULL) != 0 ||
        pthread_mutex_init(&log_mutex, NULL) != 0)
    {
        perror("Mutex initialization failed");
        return 1;
    }

    sem_init(&thread_pool_sem, 0, MAX_THREADS);
    init_buffer_queue(&chunk_queue, 10);

    curl_global_init(CURL_GLOBAL_ALL);

    pthread_t scheduler;
    if (pthread_create(&scheduler, NULL, scheduler_thread, NULL) != 0)
    {
        perror("Failed to create scheduler thread");
        return 1;
    }

    pthread_t writer_thread;
    if (pthread_create(&writer_thread, NULL, file_writer_thread, NULL) != 0)
    {
        perror("Failed to create file writer thread");
        return 1;
    }

    while (running)
    {
        print_menu();

        int option;
        if (scanf("%d", &option) != 1)
        {
            printf("Invalid input\n");
            while (getchar() != '\n')
                ;
            continue;
        }

        switch (option)
        {
        case 1:
            view_downloads();
            break;
        case 2:
        {
            char url[1024] = {0};
            char filename[256] = {0};
            int priority = 1;

            printf("Enter URL to download: ");
            scanf("%1023s", url);
            printf("Enter filename: ");
            scanf(" %255[^\n]", filename);
            printf("Enter priority (1-10, higher is more important): ");
            scanf("%d", &priority);

            priority = (priority < 1) ? 1 : (priority > 10) ? 10
                                                            : priority;

            add_download(url, filename[0] ? filename : NULL, priority);
            break;
        }
        case 3:
        {
            view_downloads();
            if (num_downloads > 0)
            {
                int index;
                printf("Enter download number to pause: ");
                scanf("%d", &index);
                if (index > 0 && index <= num_downloads)
                {
                    pause_download(&downloads[index - 1]);
                }
                else
                {
                    printf("Invalid download number\n");
                }
            }
            break;
        }
        case 4:
        {
            view_downloads();
            if (num_downloads > 0)
            {
                int index;
                printf("Enter download number to resume: ");
                scanf("%d", &index);
                if (index > 0 && index <= num_downloads)
                {
                    resume_download(&downloads[index - 1]);
                }
                else
                {
                    printf("Invalid download number\n");
                }
            }
            break;
        }
        case 5:
        {
            view_downloads();
            if (num_downloads > 0)
            {
                int index;
                printf("Enter download number to cancel: ");
                scanf("%d", &index);
                if (index > 0 && index <= num_downloads)
                {
                    cancel_download(&downloads[index - 1]);
                }
                else
                {
                    printf("Invalid download number\n");
                }
            }
            break;
        }
        case 6:
            view_logs();
            break;
        case 7:
            running = false;
            break;
        default:
            printf("Invalid option\n");
        }

        cleanup_downloads();
    }

    cleanup_downloads();

    pthread_join(scheduler, NULL);
    pthread_join(writer_thread, NULL);

    pthread_mutex_destroy(&lock);
    pthread_mutex_destroy(&scheduler_lock);
    pthread_mutex_destroy(&log_mutex);
    sem_destroy(&thread_pool_sem);

    free(chunk_queue.buffers);

    curl_global_cleanup();

    printf("Goodbye!\n");
    return 0;
}     