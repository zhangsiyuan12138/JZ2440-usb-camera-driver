#include <sys/types.h>          
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>




int main(void)
{
   //定义socket
   struct sockaddr_in server_sock_addr;
   struct sockaddr_in client_sock_addr;
   
   //定义socket句柄
   int server_fd;

   //函数专用返回值
   int ret;

   //定义接收数据缓冲区大小
   char recv_buff[1000];
   int sock_size = sizeof(struct sockaddr_in);
   int recv_len;

   
   /*socket*/
   server_fd = socket(AF_INET,SOCK_DGRAM,0);
   if(server_fd == -1){printf("request socket failed !\n"); return -1; };

   /*设置内存地址*/
   server_sock_addr.sin_addr.s_addr = INADDR_ANY ;
   server_sock_addr.sin_family =AF_INET;
   server_sock_addr.sin_port = htons(6666);
   memset(server_sock_addr.sin_zero,0,8);

   /*bind*/
   ret = bind(server_fd,(struct sockaddr *)&server_sock_addr,sizeof(struct sockaddr));
   if(ret == -1){printf("bind port and node failed !\n");}

   while(1)
   	{
   	/*recvfrom*/
       recv_len = recvfrom(server_fd,recv_buff,999,0,(struct sockaddr *)&client_sock_addr,&sock_size);
       if(recv_len > 0)
       	{
           recv_buff[recv_len] = '\0';
           printf("recieve data from %s : %s ",inet_ntoa(client_sock_addr.sin_addr),recv_buff);
	    }
    }
  close(server_fd);
   return 0;
}










































