#include <sys/types.h>   
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>



int main(int argc ,char** argv)
{

  //����һ��socket 
  struct sockaddr_in server_sock_addr;
  
  //����socket���
  int client_fd;

  //����ֵ
  int ret=0;

  //���������
  char send_buff[1000];

  if(argc != 2) { printf("usage : %s <serverip>",argv[0]);}

  /* �׽��� */ 
  client_fd = socket(AF_INET,SOCK_STREAM,0);
  if(client_fd < 0) {printf("request socket failed !\n"); return -1;}

  /* ���������ڴ��ַ           �������õ�socket������ָ ϣ�����ӵķ�����IP�Ͷ˿ں���Ϣ��IP��ַ�����û������룬��ˣ���������úͷ����������ã�Ҫ���������ϵ�һ��*/
  inet_aton(argv[1], &server_sock_addr.sin_addr);
  server_sock_addr.sin_family = AF_INET;
  server_sock_addr.sin_port = htons(6666);
  memset(server_sock_addr.sin_zero, 0, 8);

  /* connect ������������Զ�̷�������������sockfd��socket��������serv_addr��ָ���ķ�����IP�Ͷ˿ں���ȥ*/
  ret = connect(client_fd,( struct sockaddr *)&server_sock_addr,sizeof(struct sockaddr));
  if(ret == -1) {printf("connect to server failed ! \n");  return -1; }

  while(1)
  	{
       if(fgets(send_buff,999,stdin))
	     {
  /*�����������ݸ�ָ����Զ������*/
           ret = send(client_fd,send_buff,strlen(send_buff),0);     
           if(ret < 0){close(client_fd);return -1;}
		 }
    }
  return 0;
}































