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
int calcular_min_robots_para_rango(EstadoSistema *estado, float area_caja, int N_mangos_base, int robots_maximos);
int crear_cajas(EstadoSistema *estado, float area_caja, int robots_maximos);
void acomodarEnGrilla(Caja *caja);
void escanear(EstadoSistema *estado);
void cleanup_estado(EstadoSistema *estado);
int enviar_estado(int sock, EstadoSistema *estado, int robots_maximos);
int send_all(int sock, const void *buffer, size_t length);

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // Ignorar SIGPIPE para que write()/send() devuelvan error en vez de terminar proceso
    signal(SIGPIPE, SIG_IGN);

    EstadoSistema estado = {0};
    float area_caja = 500.0f;
    int robots_maximos = 10;
    int flag_P = 1; // si 1 usa parámetros por defecto, si 0 pide por stdin

    srand((unsigned)time(NULL));

    // parsear -E para pedir entrada interactiva
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-E") == 0) flag_P = 0;
    }

    if (flag_P) {
        estado.velocidad_banda = 2.0f;   // cm/s
        estado.longitud_banda = 700.0f;  // cm
        area_caja = 500.0f;              // cm^2
        estado.num_cajas = 3;
        robots_maximos = 10;
    } else {
        printf("Velocidad banda (cm/s): ");
        if (scanf("%f", &estado.velocidad_banda) != 1) exit(EXIT_FAILURE);
        printf("Longitud banda (cm): ");
        if (scanf("%f", &estado.longitud_banda) != 1) exit(EXIT_FAILURE);
        printf("Numero de cajas: ");
        if (scanf("%d", &estado.num_cajas) != 1) exit(EXIT_FAILURE);
        printf("Area de la caja (cm^2): ");
        if (scanf("%f", &area_caja) != 1) exit(EXIT_FAILURE);
        printf("Numero maximo de robots disponibles: ");
        if (scanf("%d", &robots_maximos) != 1) exit(EXIT_FAILURE);
    }

    // validar entradas
    if (estado.velocidad_banda <= 0.0f || estado.longitud_banda <= 0.0f || estado.num_cajas <= 0 || robots_maximos <= 0) {
        fprintf(stderr, "Parametros invalidos.\n");
        exit(EXIT_FAILURE);
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

    // crear cajas y calcular robots necesarios
    if (crear_cajas(&estado, area_caja, robots_maximos) != 0) {
        fprintf(stderr, "crear_cajas fallo\n");
        close(client_sockfd);
        close(server_sockfd);
        cleanup_estado(&estado);
        exit(EXIT_FAILURE);
    }

    escanear(&estado);

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
            printf("Cliente solicitó terminar.\n");
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

    // calcular número mínimo de robots necesarios según convención del proyecto
    int nrobots = calcular_min_robots_para_rango(estado, area_caja, num_mangos_base, robots_maximos);
    if (nrobots < 0) {
        // si no factible, asignamos robots_maximos (mejor intentar)
        nrobots = robots_maximos;
        printf("Aviso: cálculo devolvió no factible; usando robots_maximos = %d\n", robots_maximos);
    }
    estado->num_robots = nrobots;

    // reservar cajas
    estado->cajas = (Caja *) malloc(sizeof(Caja) * (size_t)estado->num_cajas);
    if (!estado->cajas) {
        perror("malloc(cajas)");
        return 1;
    }

    for (int i = 0; i < estado->num_cajas; i++) {
        estado->cajas[i].id = i + 1;
        estado->cajas[i].area_caja = area_caja;

        int extra = (int)ceilf(num_mangos_base * 0.2f);
        if (extra < 1) extra = 1;
        estado->cajas[i].num_mangos = num_mangos_base + (rand() % extra);

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
int calcular_min_robots_para_rango(EstadoSistema *estado, float area_caja, int N_mangos_base, int robots_maximos) {
    if (!estado || robots_maximos <= 0) return -1;
    if (area_caja <= 0.0f || N_mangos_base <= 0) return -1;

    // peor caso: 20% adicional
    int N_max = (int)ceil(N_mangos_base * 1.2);

    double velocidad_banda = (double)estado->velocidad_banda;
    double longitud_banda = (double)estado->longitud_banda;
    double tiempo_total_banda = longitud_banda / velocidad_banda; // tiempo que tarda una caja en recorrer toda la banda

    // lado de la caja (cuadrada)
    double lado = sqrt((double)area_caja);
    double distancia_diagonal = lado * sqrt(2.0);

    // velocidad del brazo (convencion): v_brazo = lado / CONST_VEL
    double v_brazo = lado / CONST_VEL;
    if (v_brazo <= 0.0) v_brazo = 1.0;

    // tiempo moviendo por diagonal (peor caso)
    double T_diagonal = distancia_diagonal / v_brazo;
    double T_total = T_diagonal + T_ETIQUETA;

    // ventana por robot si se usan robots_maximos (convención asumida)
    double T_ventana_base = tiempo_total_banda / (double)robots_maximos;
    if (T_total <= 0.0) return -1;

    double cap_por_robot_base = T_ventana_base / T_total;
    if (cap_por_robot_base <= 0.0) {
        // Un robot no alcanza ni 1 mango en su ventana (seguramente no factible)
        return -1;
    }

    // Buscar mínimo M
    for (int M = 1; M <= robots_maximos; ++M) {
        double capacidad_total = cap_por_robot_base * (double)M;
        if (capacidad_total >= (double)N_max) {
            return M;
        }
    }

    return -1;
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
        printf("\nCaja #%d (Área %.2f cm², %d mangos)\n", c->id, c->area_caja, c->num_mangos);
        for (int j = 0; j < c->num_mangos; ++j) {
            Mango *m = &c->mangos[j];
            printf(" Mango %2d | área %.1f cm² | pos (%.2f, %.2f)\n", m->id, m->area, m->x, m->y);
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

