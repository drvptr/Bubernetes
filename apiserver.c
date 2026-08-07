#include "apiserver.h"

#define MAX_NAME_LEN 64

enum { READY, PENDING, UNHELATHY, UNREACHEBLE };
enum { KIND, SPEC, STATUS, M_SIZE };
enum { NODE, POD, STATIC };

struct res {
    int val[M_SIZE];
};


