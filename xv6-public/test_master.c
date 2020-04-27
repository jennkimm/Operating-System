#include "types.h"
#include "stat.h"
#include "user.h"

#define CNT_CHILD   4

#define NAME_CHILD_STRIDE   "test_stride"
#define NAME_CHILD_MLFQ    "test_mlfq"

char* child_argv[CNT_CHILD][3] = {
    {NAME_CHILD_STRIDE, "10", 0},
    {NAME_CHILD_STRIDE, "40", 0},
    {NAME_CHILD_MLFQ, "0", 0},
    {NAME_CHILD_MLFQ, "1", 0},
};

int
main(int argc, char* argv[]){
    int pid;
    int i;

    for(int i=0;i<CNT_CHILD;i++){
        pid=fork();
        if(pid>0){
            //parent
            continue;
        }
        else if(pid==0){
            //child
            exec(child_argv[i][0], child_argv[i]);
            printf(1,"exec faild!!\n");
            exit();
        }
        else{
            printf(1, "fork failed!!\n");
            exit();
        }
    }

    for(i=0; i<CNT_CHILD;i++){
        wait();
    }
    
    exit();
}

