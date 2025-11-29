// escaner.c - servidor "scanner" para la simulación MangoNeado
// Compilar: gcc escaner.c -o escaner -lm

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "datos.h"   // Debe contener Mango, Caja, EstadoSistema

// ---- Configurables / Constantes ----
#define CONST_VEL 10.0      // velocidad relativa (se usa en cliente)
#define T_ETIQUETA 0.5      // s por pegar etiqueta (informativo)
#define MAX_ROBOTS 200
#define AREA_MANGO_PROM 90  // área promedio aproximada (cm^2)
#define SERVER_PORT 7734

// Prototipos
int calcular_min_robots_para_rango(EstadoSistema *estado, float area_caja, int robots_maximos);
int crear_cajas(EstadoSistema *estado, float area_caja, int robots_maximos);
void acomodarEnGrilla(Caja *caja);
void escanear(EstadoSistema *estado);
void cleanup_estado(EstadoSistema *estado);
int enviar_estado(int sock, EstadoSistema *estado, int robots_maximos);
int send_all(int sock, const void *buffer, size_t length);
void limpiarBuffer();
float pedirFloat(const char *mensaje);
int pedirInt(const char *mensaje);

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // Ignorar SIGPIPE para que write()/send() devuelvan error en vez de terminar proceso
    signal(SIGPIPE, SIG_IGN);

    // ------ Configuración inicial de parámetros ------
	EstadoSistema estado = {0};
	float area_caja;
	int robots_maximos;
	int flag_P = 1; // si 1 usa parámetros por defecto, si 0 pide por stdin
	
	srand((unsigned)time(NULL));
	
	// parsear -E para pedir entrada interactiva
	for (int i = 1; i < argc; ++i) {
	    if (strcmp(argv[i], "-E") == 0) flag_P = 0;
	}
	
	int parametros_validos = 0;
	while (!parametros_validos) {
	    // Pedir o asignar parámetros
	    if (flag_P) {
	        estado.velocidad_banda = 5.0f;
	        estado.longitud_banda = 700.0f;
	        area_caja = 500.0f;
	        estado.num_cajas = 3;
	        robots_maximos = 10;
	    } else {
	        estado.velocidad_banda = pedirFloat("Velocidad banda (cm/s): ");
	        estado.longitud_banda = pedirFloat("Longitud banda (cm): ");
	        estado.num_cajas = pedirInt("Numero de cajas: ");
	        area_caja = pedirFloat("Area de la caja (cm^2): ");
	        robots_maximos = pedirInt("Numero maximo de robots disponibles: ");
	    }
	
	    // validar entradas básicas
	    if (estado.velocidad_banda <= 0.0f || estado.longitud_banda <= 0.0f ||
	        estado.num_cajas <= 0 || robots_maximos <= 0) {
	        printf("Parametros invalidos. Intenta nuevamente.\n");
	        continue;
	    }
	
	    // limpiar estado previo si existiera
	    cleanup_estado(&estado);
	
	    // crear cajas
	    if (crear_cajas(&estado, area_caja, robots_maximos) != 0) {
	        printf("Error al crear cajas. Intenta nuevamente.\n");
	        cleanup_estado(&estado);
	        continue;
	    }
		
	    // escanear y ubicar mangos
	    escanear(&estado);

	    // calcular robots mínimos
	    int nrobots = calcular_min_robots_para_rango(&estado, area_caja, robots_maximos);
	    if (nrobots < 0) {
	        printf("No es posible etiquetar todos los mangos con los parámetros ingresados. Intenta nuevamente.\n");
	        cleanup_estado(&estado);
	        continue;
	    }
	
	    estado.num_robots = nrobots;
	    parametros_validos = 1;  // todo correcto, salir del loop
	}


	
	// Crear socket servidor
    int server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockfd < 0) {
        perror("socket()");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERVER_PORT);

    if (bind(server_sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("bind()");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_sockfd, 5) < 0) {
        perror("listen()");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Servidor escaner iniciado en puerto %d. Esperando cliente...\n", SERVER_PORT);

    // Accept (bloqueante)
    struct sockaddr_in client_address;
    socklen_t client_len = sizeof(client_address);
    int client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address, &client_len);
    if (client_sockfd < 0) {
        perror("accept()");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }

    char client_ip[INET_ADDRSTRLEN] = "desconocido";
    inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip));
    printf("Cliente conectado desde %s:%d\n", client_ip, ntohs(client_address.sin_port));
	
    // enviar estado al cliente
    if (enviar_estado(client_sockfd, &estado, robots_maximos) != 0) {
        fprintf(stderr, "Error enviando estado\n");
        close(client_sockfd);
        close(server_sockfd);
        cleanup_estado(&estado);
        exit(EXIT_FAILURE);
    }

    /* Enviar periódicamente num_robots calculado hasta que el cliente mande 'X' */
    while (1) {
        int to_send = estado.num_robots;
        if (send_all(client_sockfd, &to_send, sizeof(to_send)) < 0) {
            perror("send(num_robots)");
            printf("Cliente desconectado o error de envío. Saliendo loop.\n");
            break;
        }
        // esperar 100 ms por respuesta (como tu cliente)
        char ch;
        ssize_t nr = recv(client_sockfd, &ch, 1, 0);
        if (nr <= 0) {
            perror("recv()");
            printf("Cliente desconectado o error de recepción. Saliendo.\n");
            break;
        }
        printf("Mensaje recibido del cliente: %c\n", ch);
        if (ch == 'X') {
            printf("Cliente solicito terminar.\n");
            break;
        }
        usleep(100000);
    }

    // limpieza y cierre
    shutdown(client_sockfd, SHUT_RDWR);
    close(client_sockfd);
    close(server_sockfd);

    cleanup_estado(&estado);
    printf("Servidor finalizado correctamente.\n");
    return 0;
}

// -----------------------------------------------------------------------------
// crear_cajas: reserva memoria para cajas y rellena num_mangos aleatorios
// -----------------------------------------------------------------------------
int crear_cajas(EstadoSistema *estado, float area_caja, int robots_maximos) {
    if (!estado) return 1;

    // estimación de mangos base por área
    int num_mangos_base = (int)(area_caja / AREA_MANGO_PROM);
    if (num_mangos_base < 1) num_mangos_base = 1;

    // reservar cajas
    estado->cajas = (Caja *) malloc(sizeof(Caja) * (size_t)estado->num_cajas);
    if (!estado->cajas) {
        perror("malloc(cajas)");
        return 1;
    }

    for (int i = 0; i < estado->num_cajas; i++) {
        estado->cajas[i].id = i + 1;
        estado->cajas[i].area_caja = area_caja;
        estado->cajas[i].num_mangos = num_mangos_base + rand() % (int)(num_mangos_base * 0.2 + 1);

        estado->cajas[i].mangos = (Mango *) malloc(sizeof(Mango) * (size_t)estado->cajas[i].num_mangos);
        if (!estado->cajas[i].mangos) {
            perror("malloc(mangos)");
            // liberar ya asignado
            for (int j = 0; j < i; j++) free(estado->cajas[j].mangos);
            free(estado->cajas);
            estado->cajas = NULL;
            return 1;
        }
        // inicializar campos
        for (int j = 0; j < estado->cajas[i].num_mangos; j++) {
            estado->cajas[i].mangos[j].id = j + 1;
            estado->cajas[i].mangos[j].etiquetado = 0;
            estado->cajas[i].mangos[j].area = 0.0f;
            estado->cajas[i].mangos[j].x = 0.0f;
            estado->cajas[i].mangos[j].y = 0.0f;
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// calcular_min_robots_para_rango
// Modelo (conservador, compatible con tu código previo):
// - Se usa una ventana T_ventana calculada con robots_maximos: T_ventana = tiempo_maximo / robots_maximos
// - Velocidad del brazo (convención) v_brazo = lado / CONST_VEL
// - Tiempo por mango T_total = T_move_diagonal + T_ETIQUETA (approx diagonal para peor caso)
// - Capacidad por robot = T_ventana / T_total
// - Buscar minimo M (1..robots_maximos) tal que M * cap_por_robot >= N_max
// -----------------------------------------------------------------------------
int calcular_min_robots_para_rango(EstadoSistema *estado, float area_caja, int robots_maximos) {
	
    if (!estado || estado->cajas[0].num_mangos <= 0) return -1;

    float lado = sqrt(estado->cajas[0].area_caja);
    float v_brazo = lado / CONST_VEL;   // cm/s

    // tiempo que la caja pasa en el rango de un robot
    float tiempo_ventana_total = estado->longitud_banda / estado->velocidad_banda;
    
    float tiempo_ventana =  tiempo_ventana_total / robots_maximos;

    // calcular tiempo REAL para etiquetar todos los mangos
    float tiempo_total_robot = 0.0f;
	float temporal = 0.0f;
    // comienzo en el centro
    float cx = 0.0f, cy = 0.0f;
	
	int robots_necesarios = 1;

    for (int i = 0; i < estado->cajas[0].num_mangos; i++) {

        float mx = estado->cajas[0].mangos[i].x;
        float my = estado->cajas[0].mangos[i].y;

        float dx = mx - cx;
        float dy = my - cy;

        float dist = sqrt(dx*dx + dy*dy);

        float t_mov = dist / v_brazo;
		
		temporal += t_mov + T_ETIQUETA;
		if(temporal > tiempo_ventana){
			temporal = 0.0f;
			cx = 0.0f;
			cy = 0.0f;
			robots_necesarios += 1;
			
			mx = estado->cajas[0].mangos[i].x;
	        my = estado->cajas[0].mangos[i].y;
	        dx = mx - cx;
	        dy = my - cy;
	        dist = sqrt(dx*dx + dy*dy);
	        t_mov = dist / v_brazo;
	        temporal += t_mov + T_ETIQUETA;
	        tiempo_total_robot += t_mov + T_ETIQUETA;
		}else{
			tiempo_total_robot += t_mov + T_ETIQUETA;
			// ahora el brazo queda en este mango
	        cx = mx;
	        cy = my;
		}
        
    }
    
    if (robots_necesarios > robots_maximos){
		robots_necesarios = -1;
	}
        
    return robots_necesarios;
}

// -----------------------------------------------------------------------------
// acomodarEnGrilla: sitúa los mangos de manera determinista en una grilla
// -----------------------------------------------------------------------------
void acomodarEnGrilla(Caja *caja) {
    if (!caja) return;
    float lado = sqrtf(caja->area_caja);
    int N = caja->num_mangos;
    if (N <= 0) return;

    int celdas = (int)ceilf(sqrtf((float)N));
    if (celdas <= 0) celdas = 1;
    float tamCelda = lado / (float)celdas;

    int index = 0;
    for (int fila = 0; fila < celdas && index < N; fila++) {
        for (int col = 0; col < celdas && index < N; col++) {
            Mango *m = &caja->mangos[index];
            // área aleatoria entre 70 y 90 (para variación)
            m->area = 70.0f + (float)(rand() % 21);
            float cx = (col + 0.5f) * tamCelda;
            float cy = (fila + 0.5f) * tamCelda;
            // coordenadas relativas al centro de la caja
            m->x = cx - lado / 2.0f;
            m->y = cy - lado / 2.0f;
            index++;
        }
    }
}

// -----------------------------------------------------------------------------
// escanear: asigna posiciones a los mangos e imprime
// -----------------------------------------------------------------------------
void escanear(EstadoSistema *estado) {
    if (!estado || !estado->cajas) return;
    printf("\n=== INICIANDO ESCANEO DE CAJAS ===\n");
    for (int i = 0; i < estado->num_cajas; ++i) {
        Caja *c = &estado->cajas[i];
        // asegurar ids y estados
        for (int j = 0; j < c->num_mangos; ++j) {
            c->mangos[j].id = j + 1;
            c->mangos[j].etiquetado = 0;
        }
        acomodarEnGrilla(c);
        printf("\nCaja #%d (Area %.2f cm^2, %d mangos)\n", c->id, c->area_caja, c->num_mangos);
        for (int j = 0; j < c->num_mangos; ++j) {
            Mango *m = &c->mangos[j];
            printf(" Mango %2d | area %.1f cm^2 | pos (%.2f, %.2f)\n", m->id, m->area, m->x, m->y);
        }
    }
    printf("\n=== ESCANEO COMPLETADO ===\n");

}

// -----------------------------------------------------------------------------
// cleanup_estado
// -----------------------------------------------------------------------------
void cleanup_estado(EstadoSistema *estado) {
    if (!estado) return;
    if (estado->cajas) {
        for (int i = 0; i < estado->num_cajas; ++i) {
            if (estado->cajas[i].mangos) {
                free(estado->cajas[i].mangos);
                estado->cajas[i].mangos = NULL;
            }
        }
        free(estado->cajas);
        estado->cajas = NULL;
    }
}

// -----------------------------------------------------------------------------
// enviar_estado: serializa y envía EstadoSistema por socket
// Formato (coincide con el cliente):
// 1) velocidad_banda (float)
// 2) longitud_banda (float)
// 3) num_robots (int32_t)
// 4) num_cajas (int32_t)
// 5) robots_maximos (int)  <-- enviado como entero simple
// Para cada caja:
//   id (int32_t), area (float), num_mangos (int32_t)
//   para cada mango: id (int32_t), x (float), y (float), area (float), etiquetado (int32_t)
// -----------------------------------------------------------------------------
int enviar_estado(int sock, EstadoSistema *estado, int robots_maximos) {
	printf("Esperando velocidad_banda...\n");
    if (!estado) return -1;
    float f;
    int32_t i32;

    f = (float) estado->velocidad_banda;
    if (send_all(sock, &f, sizeof(f)) < 0) return -1;

    f = (float) estado->longitud_banda;
    if (send_all(sock, &f, sizeof(f)) < 0) return -1;

    i32 = (int32_t) estado->num_robots;
    if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;

    i32 = (int32_t) estado->num_cajas;
    if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;

    /* enviar robots_maximos (int) */
    if (send_all(sock, &robots_maximos, sizeof(int)) < 0) return -1;

    for (int c = 0; c < estado->num_cajas; ++c) {
        Caja *caja = &estado->cajas[c];

        i32 = (int32_t) caja->id;
        if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;

        f = (float) caja->area_caja;
        if (send_all(sock, &f, sizeof(f)) < 0) return -1;

        i32 = (int32_t) caja->num_mangos;
        if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;

        for (int m = 0; m < caja->num_mangos; ++m) {
            Mango *mg = &caja->mangos[m];

            i32 = (int32_t) mg->id;
            if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;

            f = mg->x;
            if (send_all(sock, &f, sizeof(f)) < 0) return -1;

            f = mg->y;
            if (send_all(sock, &f, sizeof(f)) < 0) return -1;

            f = mg->area;
            if (send_all(sock, &f, sizeof(f)) < 0) return -1;

            i32 = (int32_t) mg->etiquetado;
            if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// send_all: envía todo el buffer
// -----------------------------------------------------------------------------
int send_all(int sock, const void *buffer, size_t length) {
    const char *ptr = (const char *)buffer;
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent = send(sock, ptr + total_sent, length - total_sent, 0);
        if (sent <= 0) return -1;
        total_sent += (size_t)sent;
    }
    return 0;
}


// Función para limpiar el buffer
void limpiarBuffer() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

// Función genérica para pedir un float válido
float pedirFloat(const char *mensaje) {
    float valor;
    while (1) {
        printf("%s", mensaje);
        if (scanf("%f", &valor) == 1) {
            limpiarBuffer();
            return valor;
        }
        printf("Entrada inválida. Intenta nuevamente.\n");
        limpiarBuffer();
    }
}

// Función genérica para pedir un entero válido
int pedirInt(const char *mensaje) {
    int valor;
    while (1) {
        printf("%s", mensaje);
        if (scanf("%d", &valor) == 1) {
            limpiarBuffer();
            return valor;
        }
        printf("Entrada inválida. Intenta nuevamente.\n");
        limpiarBuffer();
    }
}

