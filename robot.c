#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "datos.h"

EstadoSistema *recibir_estado(int sock);
void imprimir_estado_sistema(EstadoSistema *estado);
void imprimir_caja(Caja *caja);
void imprimir_mango(Mango *mango);
int recv_all(int sock, void *buffer, size_t length);

int main(){
	
		//EstadoSistema estado;
	    int sockfd;
        struct sockaddr_in client_address;
        int len;
        int result;
        char ch;
        int num_mangos;
        EstadoSistema *estado;

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

		estado = recibir_estado(sockfd);
	
		imprimir_estado_sistema(estado);
			
		read(sockfd, &num_mangos, sizeof(int));
		printf("Llego al cliente, numero de robots: %d\n", num_mangos);
		ch = 'X';
		write(sockfd, &ch, 1);
		printf("Cliente terminado");
        close(sockfd);
        exit(0);
}

EstadoSistema *recibir_estado(int sock) {
    float f;
    int32_t i32;

    EstadoSistema *estado = malloc(sizeof(EstadoSistema));
    if (!estado) return NULL;

    // inicializar
    memset(estado, 0, sizeof(*estado));

    // 1) velocidad_banda
    if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
    estado->velocidad_banda = (double) f;

    // 2) longitud_banda
    if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
    estado->longitud_banda = (double) f;

    // 3) num_robots
    if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
    estado->num_robots = (int) i32;

    // 4) num_cajas
    if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
    estado->num_cajas = (int) i32;

    // reservar cajas
    if (estado->num_cajas <= 0) goto fail;
    estado->cajas = malloc(sizeof(Caja) * (size_t)estado->num_cajas);
    if (!estado->cajas) goto fail;

    for (int c = 0; c < estado->num_cajas; ++c) {
        Caja *caja = &estado->cajas[c];
        memset(caja, 0, sizeof(*caja));

        // id
        if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
        caja->id = (int) i32;

        // area
        if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
        caja->area_caja = f;

        // num_mangos
        if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
        caja->num_mangos = (int) i32;

        if (caja->num_mangos < 0) goto fail;

        // reservar mangos
        caja->mangos = malloc(sizeof(Mango) * (size_t)caja->num_mangos);
        if (!caja->mangos) goto fail;

        for (int m = 0; m < caja->num_mangos; ++m) {
            Mango *mg = &caja->mangos[m];

            if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
            mg->id = (int) i32;

            if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
            mg->x = f;

            if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
            mg->y = f;

            if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
            mg->area = f;

            if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
            mg->etiquetado = (int) i32;
        }
    }

    return estado;

fail:
    // liberar memoria en fallo
    if (estado) {
        if (estado->cajas) {
            for (int j = 0; j < estado->num_cajas; ++j) {
                free(estado->cajas[j].mangos);
            }
            free(estado->cajas);
        }
        free(estado);
    }
    return NULL;
}

void imprimir_mango(Mango *mango) {
    printf("Mango ID: %d\n", mango->id);
    printf("Posición X: %.2f cm\n", mango->x);
    printf("Posición Y: %.2f cm\n", mango->y);
    printf("Área: %.2f cm²\n", mango->area);
    printf("Etiquetado: %s\n", mango->etiquetado ? "Sí" : "No");
}

// Función para imprimir una caja
void imprimir_caja(Caja *caja) {
    printf("Caja ID: %d\n", caja->id);
    printf("Área de la caja: %.2f cm²\n", caja->area_caja);
    printf("Número de mangos: %d\n", caja->num_mangos);
    for (int i = 0; i < caja->num_mangos; i++) {
        printf("  Mango %d:\n", i + 1);
        imprimir_mango(&caja->mangos[i]);
    }
}

// Función para imprimir el estado del sistema
void imprimir_estado_sistema(EstadoSistema *estado) {
    printf("Velocidad de la banda: %.2f cm/s\n", estado->velocidad_banda);
    printf("Longitud de la banda: %.2f cm\n", estado->longitud_banda);
    printf("Número de robots: %d\n", estado->num_robots);
    printf("Número de cajas: %d\n", estado->num_cajas);
    for (int i = 0; i < estado->num_cajas; i++) {
        printf("Caja %d:\n", i + 1);
        imprimir_caja(&estado->cajas[i]);
    }
}

int recv_all(int sock, void *buffer, size_t length) {
    char *ptr = buffer;
    size_t total_received = 0;

    while (total_received < length) {
        ssize_t rec = recv(sock, ptr + total_received, length - total_received, 0);
        if (rec <= 0) return -1;
        total_received += rec;
    }

    return 0;
}


