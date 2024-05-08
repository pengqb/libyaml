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
#define ACTION_DEL 1 << 1
#define ACTION_MODIFY (ACTION_ADD | ACTION_DEL)

#define ACTION_ADD_STR "add"
#define ACTION_DEL_STR "del"
#define ACTION_MODIFY_STR "modify"

#define TASK_CHECK "check"
#define TASK_UPGRADE "upgrade"
#define TASK_ROLLBACK "rollback"

#define PATH_MAX 256
#define ITEM_MAX 128

#define UPGRADE_BASE "/Users/pengqiangbing/work/opensource/yaml_data/upgrade/"
#define WAF_BASE "/Users/pengqiangbing/work/opensource/yaml_data/waf/"
#define BAK_BASE "/Users/pengqiangbing/work/opensource/yaml_data/bak/"

//#define UPGRADE_BASE "./upgrade/"
//#define WAF_BASE "/waf/"
//#define BAK_BASE "./bak/"

// 定义与YAML中结构对应的C结构体
typedef struct FileItem {
    const char *relative_path;
    int chmod;
    int action;
} FileItem;

void free_file_item(FileItem item) {
    FREE_SAFE(item.relative_path);
}

void parse_yaml_mapping(yaml_parser_t *parser, FileItem *item);

// 函数用来创建多级目录
int mkdir_p(const char *dir_path) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    len = strlen(tmp);

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
            *p = '/';
        }
    }

    mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    return 0;
}

void bak_file(FileItem *item, const char *waf_path, const char *bak_path) {
    /*delete、modify需要备份*/
    if (item->action & ACTION_DEL) {
        /*先备份，再删除当前文件*/
        if (access(waf_path, F_OK) == 0) {
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
        int status;
        char cmdstr[2 * PATH_MAX + 10] = {0};/* 冗余2byte */
        int len = snprintf(cmdstr, 2 * PATH_MAX + 10, "cp -fr %s %s", upgrade_path, waf_path);
        status = system(cmdstr);
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
            yaml_event_delete(&event);  // 删除已处理的值事件
        } else {
            yaml_event_delete(&event);  // 删除非预期的事件
        }
    }
}


// 读取并解析YAML文件
void process_yaml_file(const char *filename, char *task) {
    FILE *input = fopen(filename, "rb");
    if (!input) {
        fprintf(stderr, "Failed to open file\n");
        return;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fprintf(stderr, "Failed to initialize parser\n");
        fclose(input);
        return;
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
    for (size_t i = 0; i < item_count; i++) {
        printf("Relative Path: %s\n", items[i].relative_path);
        printf("Chmod Permissions: %d\n", items[i].chmod);
        printf("Action: %d\n", items[i].action);

        char bak_path[PATH_MAX] = {0};
        int len = snprintf(bak_path, PATH_MAX, "%s%s", BAK_BASE, items[i].relative_path);
        if (len >= PATH_MAX - 1) {
            fprintf(stderr, "bak_file: path too long:%s%s\n", BAK_BASE, items[i].relative_path);
            return;
        }
        char waf_path[PATH_MAX] = {0};
        len = snprintf(waf_path, PATH_MAX, "%s%s", WAF_BASE, items[i].relative_path);
        if (len >= PATH_MAX - 1) {
            fprintf(stderr, "waf_file: path too long:%s%s\n", WAF_BASE, items[i].relative_path);
            return;
        }
        char upgrade_path[PATH_MAX] = {0};
        len = snprintf(upgrade_path, PATH_MAX, "%s%s", UPGRADE_BASE, items[i].relative_path);
        if (len >= PATH_MAX - 1) {
            fprintf(stderr, "upgrade_file: path too long:%s%s\n", UPGRADE_BASE, items[i].relative_path);
            return;
        }

        if (strncmp(task, TASK_UPGRADE, sizeof(TASK_UPGRADE)) == 0) {
            bak_file(&items[i], waf_path, bak_path);
            upgrade_file(&items[i], upgrade_path, waf_path);
        }else if (strncmp(task, TASK_ROLLBACK, sizeof(TASK_ROLLBACK)) == 0) {
            rollback_file(&items[i], waf_path, bak_path);
        }else if (strncmp(task, TASK_CHECK, sizeof(TASK_CHECK)) == 0){
            /*do nothing*/
        }else{
            fprintf(stderr, "unknown task:%s\n", task);
        }
    }

    for (size_t i = 0; i < item_count; i++) {
        free_file_item(items[i]);
    }
}

int main(int argc, char *argv[]) {

    if (argc < 3) {
        fprintf(stderr, "Usage: %s file.yaml check/upgrade/rollback...\n", argv[0]);
        return 0;
    }
    process_yaml_file(argv[1], argv[2]);
    return 0;
}