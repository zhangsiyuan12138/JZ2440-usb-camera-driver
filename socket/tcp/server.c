#include <sys/types.h>         
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>



int main(int argc,char** argv)
{
	//����һ�������
	int server_fd;
	int new_client_fd;

	//����һ������ֵ���
	int ret;

	//����һ��server_socket ��ַ
    struct sockaddr_in server_sock_addr;
    struct sockaddr_in client_sock_addr;

	//sockaddr����
	int sock_len = sizeof(struct sockaddr );

	//������������
	int connect_num = -1;

	//�������ݻ������ͳ���
	char recv_buff[1000];
	int  recv_len;

    //ɱ����������        server
    signal(SIGCHLD,SIG_IGN);

/* socket  ����һ���׽��� */
	server_fd = socket(AF_INET,SOCK_STREAM,0);
	if(server_fd < 0) { printf("request for socket failed!\n"); return -1;}

/* �������������ַ */
    server_sock_addr.sin_addr.s_addr = INADDR_ANY;
    server_sock_addr.sin_family = AF_INET;
    server_sock_addr.sin_port =htons(6666);
    memset(server_sock_addr.sin_zero,0,8);

/* bind ���׽��ְ󶨵����ؼ������ĳһ���˿���*/
    ret = bind(server_fd,( struct sockaddr *)&server_sock_addr, sizeof(struct sockaddr));    // socket�������׽�������û���ر����������ַ��bind���Ǹ�Ƕ����������ַ
    if(ret <0) {printf("bind server_fd and server socket failed !\n"); return -1;};

/* listen ������������˿ں�IP���ڼ���״̬���ȴ�������ĳһ�ͻ�������������*/
    ret = listen(server_fd,10);
    if(ret < 0) {printf("listen from server socket failed !\n");return -1;}

	while(1)
		{
/* accept �����������󣬽�������ͻ���֮���ͨ������(addr,addrlen���������ͻ��˵ĳ�����д��,��������ֻҪ����ָ��Ϳ�����) */		
            new_client_fd = accept(server_fd,( struct sockaddr *)&client_sock_addr, &sock_len);
            if(new_client_fd != -1)
            	{
                 connect_num++;
                 printf("clients %d  connect to this server :%s \n",connect_num,inet_ntoa(client_sock_addr.sin_addr));
                 if(!fork()) /* �ӽ��� */
				 	while(1)
				 		{
/* recv ���µ��׽���������Զ���������������ݣ��������ݴ浽�ɲ���bufָ����ڴ�ռ�*/
							  recv_len = recv(new_client_fd,recv_buff,999,0);  
		                      if(recv_len <= 0){printf("client %d exit!\n",connect_num);close(new_client_fd);return -1;}
		                      else 
							  	{ 
		                            recv_buff[recv_len] = '\0';
		                            printf("recieve data from client %d  : %s",connect_num,recv_buff);/* �˴����ü�'/n'       fgets(client.c)�ѻس�����Ҳ��ȡ��buff��*/
							    } 	
				 		}
			    } /* ������ */

	   }
       close(server_fd);
	   return 0;
}


