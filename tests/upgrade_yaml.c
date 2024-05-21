//
// Created by 彭强兵 on 2024/5/1.
// yum install libyaml-devel
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <yaml.h>

#define FREE_SAFE(p) do{ if(p){free(p);p=NULL;} }while(0)

#define ACTION_ADD 1
#define ACTION_DEL (1 << 1)
#define ACTION_MODIFY (ACTION_ADD | ACTION_DEL)

#define ACTION_ADD_STR "add"
#define ACTION_DEL_STR "del"
#define ACTION_MODIFY_STR "modify"

#define TASK_CHECK "check"
#define TASK_UPGRADE "upgrade"
#define TASK_ROLLBACK "rollback"

#define PATH_MAX 256
#define ITEM_MAX 128

#define UPGRADE_BASE "upgrade/"
#define WAF_BASE "/waf/"
#define BAK_BASE "bak/"

// 定义与YAML中结构对应的C结构体
typedef struct FileItem {
    char *relative_path;
    char *waf_base;
    int chmod;
    int action;
} FileItem;

void free_file_item(FileItem item) {
    FREE_SAFE(item.relative_path);
    FREE_SAFE(item.waf_base);
}

void parse_yaml_mapping(yaml_parser_t *parser, FileItem *item);

// 函数用来创建多级目录
int mkdir_p(const char *dir_path) {
    if (!dir_path) {
        fprintf(stderr, "Invalid directory path\n");
        return -1;
    }

    size_t path_len = strlen(dir_path) + 1;
    char *tmp = malloc(path_len);
    if (!tmp) {
        fprintf(stderr, "Memory allocation error\n");
        return -1;
    }
    snprintf(tmp, path_len, "%s", dir_path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0 && errno != EEXIST) {
                fprintf(stderr, "Failed to mkdir. strerror:%s\n", strerror(errno));
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    free(tmp);
    return 0;
}

void bak_file(FileItem *item, const char *waf_path, const char *bak_path) {
    /*delete、modify需要备份*/
    if (item->action & ACTION_DEL) {
        /*先备份，再删除当前文件*/
        if (access(waf_path, F_OK) == 0) {
            mkdir_p(bak_path);
            if (rename(waf_path, bak_path) != 0) {
                fprintf(stderr, "Failed to rename file:%s to%s. strerror:%s\n", waf_path, bak_path, strerror(errno));
                return;
            }
        }
    }
}

void upgrade_file(FileItem *item, const char *upgrade_path, const char *waf_path) {
    /*add、modify需要升级*/
    if (item->action & ACTION_ADD) {
        char cmdstr[2 * PATH_MAX + 10] = {0};/* 冗余2byte */
        int len = snprintf(cmdstr, 2 * PATH_MAX + 10, "cp -fr %s %s", upgrade_path, waf_path);
        if (len >= 2 * PATH_MAX + 10) {
            fprintf(stderr, "cp cmd too long:%s...\n", cmdstr);
            return;
        }

        int status = system(cmdstr);
        if (status < 0) {
            fprintf(stderr, "Failed to shell: %s. strerror:%s\n", cmdstr, strerror(errno));
            return;
        }
    }
}

void rollback_file(FileItem *item, const char *waf_path, const char *bak_path) {
    /*add、modify需要删除*/
    if (item->action & ACTION_ADD) {
        if (access(waf_path, F_OK) == 0) {
            if (unlink(waf_path) != 0) {
                fprintf(stderr, "Failed to delete file: %s. strerror:%s\n", waf_path, strerror(errno));
                return;
            }
        }
    }

    /*del、modify需要回滚*/
    if (item->action & ACTION_DEL) {
        /*先备份，再删除当前文件*/
        if (access(bak_path, F_OK) == 0) {
            if (rename(bak_path, waf_path) != 0) {
                fprintf(stderr, "Failed to rename file:%s to%s. strerror:%s\n", bak_path, waf_path, strerror(errno));
                return;
            }
        }
    }
}

// 解析单个YAML映射节点
void parse_yaml_mapping(yaml_parser_t *parser, FileItem *item) {
    yaml_event_t event;
    while (1) {
        // 获取下一个事件
        if (!yaml_parser_parse(parser, &event)) {
            fprintf(stderr, "Unable to retrieve event. strerror:%s\n", strerror(errno));
            return;
        }

        // 处理映射内的键值对
        if (event.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&event);
            break;  // 如果遇到映射结束事件，则退出循环
        } else if (event.type == YAML_SCALAR_EVENT) {
            const char *key = (const char *) event.data.scalar.value;

            // 获取关联的值事件
            if (!yaml_parser_parse(parser, &event) || event.type != YAML_SCALAR_EVENT) {
                fprintf(stderr, "Unable to retrieve event. strerror:%s\n", strerror(errno));
                continue;
            }

            if (strcmp(key, "relative_path") == 0) {
                item->relative_path = strdup((const char *) event.data.scalar.value);
            } else if (strcmp(key, "waf_base") == 0) {
                item->waf_base = strdup((const char *) event.data.scalar.value);
            } else if (strcmp(key, "chmod") == 0) {
                item->chmod = atoi((const char *) event.data.scalar.value);
            } else if (strcmp(key, "action") == 0) {
                const char *value = (const char *) event.data.scalar.value;
                if (strcmp(value, "delete") == 0) {
                    item->action = ACTION_DEL;
                } else if (strcmp(value, "add") == 0) {
                    item->action = ACTION_ADD;
                } else if (strcmp(value, "modify") == 0) {
                    item->action = ACTION_MODIFY;
                }
            }
            yaml_event_delete(&event);
        } else {
            yaml_event_delete(&event);
        }
    }
}

int safe_snprintf(char *path, char *base_path, char *relative_pateh) {
    int len = snprintf(path, PATH_MAX, "%s%s", base_path, relative_pateh);
    if (len >= PATH_MAX - 1) {
        fprintf(stderr, "path too long:%s%s\n", base_path, relative_pateh);
        return -1;
    }
    return 0;
}

// 读取并解析YAML文件
int process_yaml_file(const char *filename, char *task) {
    size_t i, res = 0;
    FILE *input = fopen(filename, "rb");
    if (!input) {
        fprintf(stderr, "Failed to open file\n");
        return -1;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fprintf(stderr, "Failed to initialize parser\n");
        fclose(input);
        return -1;
    }
    yaml_parser_set_input_file(&parser, input);

    // 初始化FileItem数组，实际应用中可能需要动态分配空间
    FileItem items[ITEM_MAX];
    size_t item_count = 0;

    yaml_event_t event;
    while (yaml_parser_parse(&parser, &event)) {
        switch (event.type) {
            case YAML_STREAM_START_EVENT:
            case YAML_DOCUMENT_START_EVENT:
                break;
            case YAML_MAPPING_START_EVENT: // 每个映射开始，都可能是新的FileItem
                if (item_count >= sizeof(items) / sizeof(items[0])) {
                    fprintf(stderr, "Exceeding preset array size. size=%d\n", ITEM_MAX);
                    break;
                }
                parse_yaml_mapping(&parser, &items[item_count]);
                item_count++;
                break;
            default:
                break;
        }
        if (parser.eof == 1) {
            yaml_event_delete(&event);
            break;
        }
        yaml_event_delete(&event);
    }

    // 清理资源
    yaml_parser_delete(&parser);
    fclose(input);

    // 打印解析后的数据
    for (i = 0; i < item_count; i++) {
//        printf("Relative Path: %s\n", items[i].relative_path);
//        printf("Chmod Permissions: %d\n", items[i].chmod);
//        printf("Action: %d\n", items[i].action);

        char bak_path[PATH_MAX] = {0};
        if (safe_snprintf(bak_path, BAK_BASE, items[i].relative_path) == -1) {
            res = -1;
            break;
        }
        char waf_path[PATH_MAX] = {0};
        if (safe_snprintf(waf_path, items[i].waf_base ? items[i].waf_base : WAF_BASE, items[i].relative_path) == -1) {
            res = -1;
            break;
        }
        char upgrade_path[PATH_MAX] = {0};
        if (safe_snprintf(upgrade_path, UPGRADE_BASE, items[i].relative_path) == -1) {
            res = -1;
            break;
        }

        if (strncmp(task, TASK_UPGRADE, strlen(TASK_UPGRADE)) == 0) {
            bak_file(&items[i], waf_path, bak_path);
            upgrade_file(&items[i], upgrade_path, waf_path);
        } else if (strncmp(task, TASK_ROLLBACK, strlen(TASK_ROLLBACK)) == 0) {
            rollback_file(&items[i], waf_path, bak_path);
        } else if (strncmp(task, TASK_CHECK, strlen(TASK_CHECK)) == 0) {
            /*do nothing*/
            printf("ok\n");
        } else {
            fprintf(stderr, "unknown task:%s\n", task);
            res = -1;
            break;
        }
    }

    for (i = 0; i < item_count; i++) {
        free_file_item(items[i]);
    }
    return res;
}

int main(int argc, char *argv[]) {

    if (argc < 3) {
        fprintf(stderr, "Usage: %s file.yaml check/upgrade/rollback...\n", argv[0]);
        return -1;
    }
    return process_yaml_file(argv[1], argv[2]);
}