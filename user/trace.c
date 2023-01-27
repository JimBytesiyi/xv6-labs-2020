#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// trace 2147483647 grep hello README
// 这个2147483647是int(有符号)的最大值

// usys.pl会自动生成system call的汇编代码
// 在这个文件中添加trace函数的入口就可以跳到kernel space执行跟踪了
// argv[0]永远是函数名

int
main(int argc, char *argv[]) // 实际上argv还可以写成char** argv
{
  int i;
  char *nargv[MAXARG];

  if(argc < 3 || (argv[1][0] < '0' || argv[1][0] > '9')){
    fprintf(2, "Usage: %s mask command\n", argv[0]);
    exit(1);
  }
  // user.h中存放的是函数的定义(函数原型)
  // 这里可以推断出trace函数的参数和返回值
  if (trace(atoi(argv[1])) < 0) {
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }
  
  for(i = 2; i < argc && i < MAXARG; i++){
    nargv[i-2] = argv[i]; // trace的调用只用了argv[0]和argv[1]
  }
  // 所以exec是用来处理argv[2]开始的函数的
  // exec函数的作用是: 将new program完全替换到当前进程的memory中
  // exec是个system call function
  exec(nargv[0], nargv);
  exit(0);
}
