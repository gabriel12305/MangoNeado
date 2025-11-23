#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "datos.h"

int main(){
	
		//EstadoSistema estado;
	    int sockfd;
        struct sockaddr_in client_address;
        int len;
        int result;
        char ch;
        int num_mangos;

        sockfd = socket(PF_INET, SOCK_STREAM, 0);
        if(sockfd < 0){
                perror("Error al crear el socket");
                exit(EXIT_FAILURE);
        }

        client_address.sin_family = AF_INET;
        client_address.sin_addr.s_addr = inet_addr("127.0.0.1");
        client_address.sin_port = htons(7734);
        len = sizeof(client_address);

        result = connect(sockfd, (struct sockaddr *) &client_address, len);
        if(result < 0){
                perror("Error en la peticion para conectarse al servidor\n");
                exit(EXIT_FAILURE);
        }

		read(sockfd, &num_mangos, sizeof(int));
		printf("Llego al cliente, numero de robots: %d\n", num_mangos);
		ch = 'X';
		write(sockfd, &ch, 1);
		printf("Cliente terminado");
        close(sockfd);
        exit(0);
}




