// escaner.c
// Servidor "scanner" para la simulación MangoNeado
// - Genera cajas y posiciones de mangos
// - Calcula número mínimo de robots activables según parámetros
// - Envía num_robots al cliente por socket hasta que cliente envíe 'X'
//
// Compilar: gcc escaner.c -o escaner -lm
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> // int32_t
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

#include "datos.h"   // Debe contener definiciones de Mango, Caja, EstadoSistema

// ---- Configurables / Constantes ----
#define CONST_VEL 10.0      // Convención: v_brazo = lado / CONST_VEL (cm/s)
#define T_ETIQUETA 0.5      // s por pegar etiqueta
#define MAX_ROBOTS 200
#define AREA_MANGO_PROM 90  // área promedio aproximada (cm^2)

// Prototipos
int calcular_min_robots_para_rango(EstadoSistema *estado, float area_caja, int N_mangos, int robots_maximos);
int crear_cajas(EstadoSistema *estado, float area_caja, int robots_maximos);
void acomodarEnGrilla(Caja *caja);
void escanear(EstadoSistema *estado);
void cleanup_estado(EstadoSistema *estado);
int enviar_estado(int sock, EstadoSistema *estado, &robots_maximos);
int send_all(int sock, const void *buffer, size_t length);

// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // Ignorar SIGPIPE para que write() devuelva -1 en lugar de terminar el proceso
    signal(SIGPIPE, SIG_IGN);

    EstadoSistema estado = {0};
    float area_caja = 500.0f;
    int robots_maximos = 10;
    char flag_P = 1;

    // inicializar RNG una sola vez
    srand((unsigned)time(NULL));

    // ---------- Socket variables ----------
    int server_sockfd = -1;
    int client_sockfd = -1;
    struct sockaddr_in server_address;
    struct sockaddr_in client_address;
    socklen_t client_len;
    int rc;

    // Parsear argumentos simples: si existe "-E" pedimos inputs
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-E") == 0) flag_P = 0;
    }

    if (flag_P == 1) {
        estado.velocidad_banda = 2.0;   // cm/s
        estado.longitud_banda = 700.0;  // cm
        area_caja = 500.0f;             // cm^2
        estado.num_cajas = 3;
        robots_maximos = 10;
    } else {
        printf("Elije la velocidad de la banda transportadora (cm/s): ");
        if (scanf("%f", &estado.velocidad_banda) != 1) exit(EXIT_FAILURE);
        printf("Elije la longitud de la banda transportadora (cm): ");
        if (scanf("%f", &estado.longitud_banda) != 1) exit(EXIT_FAILURE);
        printf("Elije el numero de cajas: ");
        if (scanf("%d", &estado.num_cajas) != 1) exit(EXIT_FAILURE);
        printf("Elije el area de la caja (cm²): ");
        if (scanf("%f", &area_caja) != 1) exit(EXIT_FAILURE);
        printf("Elije el numero maximo de robots disponibles: ");
        if (scanf("%d", &robots_maximos) != 1) exit(EXIT_FAILURE);
    }

    // Crear socket servidor
    server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockfd < 0) {
        perror("socket()");
        exit(EXIT_FAILURE);
    }

    // reusable address (evita "address already in use" en pruebas)
    int opt = 1;
    setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(7734);

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

    printf("Servidor iniciado en el puerto 7734. Esperando cliente...\n");

    // Accept: esperamos un cliente (bloqueante)
    client_len = sizeof(client_address);
    client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address, &client_len);
    if (client_sockfd < 0) {
        perror("accept()");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }

    char client_ip[INET_ADDRSTRLEN] = "desconocido";
    inet_ntop(AF_INET, &client_address.sin_addr, client_ip, sizeof(client_ip));
    printf("Cliente conectado desde %s:%d\n", client_ip, ntohs(client_address.sin_port));

    // CREAR CAJAS Y CALCULAR ROBOTS (maneja errores y libera si falla)
    if (crear_cajas(&estado, area_caja, robots_maximos) != 0) {
        fprintf(stderr, "Error: crear_cajas fallo.\n");
        // No cerramos client_sockfd si no tiene sentido; cerramos todo de forma segura
        if (client_sockfd >= 0) close(client_sockfd);
        if (server_sockfd >= 0) close(server_sockfd);
        cleanup_estado(&estado);
        exit(EXIT_FAILURE);
    }

    // Escaneo (coloca mangos dentro de cajas y muestra posiciones)
    escanear(&estado);
	
	enviar_estado(client_sockfd, &estado ,&robots_maximos);
	
    // Enviar num_robots al cliente y esperar respuesta; repetir hasta que cliente envíe 'X' o se desconecte
    while (1) {
        int to_send = estado.num_robots;
        ssize_t nw = send(client_sockfd, &to_send, sizeof(to_send), 0);
        if (nw <= 0) {
            perror("send()");
            printf("Cliente desconectado o error de envío. Cerrando conexión.\n");
            break;
        }
        // Esperamos 1 byte de respuesta
        char ch;
        ssize_t nr = recv(client_sockfd, &ch, 1, 0);
        if (nr <= 0) {
            perror("recv()");
            printf("Cliente desconectado o error de recepción. Cerrando conexión.\n");
            break;
        }
        printf("Mensaje del cliente: %c\n", ch);
        if (ch == 'X') {
            printf("Cliente solicitó terminar. Cerrando conexión.\n");
            break;
        }
        // Para evitar busy loop, esperar un poco (opcional)
        usleep(100000); // 100 ms
    }

    // Cerrar sockets
    if (client_sockfd >= 0) {
        shutdown(client_sockfd, SHUT_RDWR);
        close(client_sockfd);
    }
    if (server_sockfd >= 0) close(server_sockfd);

    // Liberar memoria de cajas/mangos
    cleanup_estado(&estado);

    printf("Servidor terminado correctamente.\n");
    return 0;
}

// -----------------------------------------------------------------------------
// Crea cajas y reserva memoria. Calcula num_robots (llama a calcular_min_robots...)
// Devuelve 0 si OK, 1 si error allocation
// -----------------------------------------------------------------------------
int crear_cajas(EstadoSistema *estado, float area_caja, int robots_maximos) {
    if (!estado) return 1;
    // Estimación básica de mangos según área promedio
    int num_mangos_base = (int)(area_caja / AREA_MANGO_PROM);
    if (num_mangos_base < 1) num_mangos_base = 1;

    // Pedimos al cálculo (N_mangos base)
    int nrobots = calcular_min_robots_para_rango(estado, area_caja, num_mangos_base, robots_maximos);
    estado->num_robots = nrobots;
    if (nrobots < 0) {
        printf("Aviso: no es posible etiquetar con los parametros dados (num_robots = %d)\n", nrobots);
        // Devolvemos error para que el main lo maneje
        // No obstante, seguimos reservando cajas para poder inspeccionarlas (opcional)
    }

    // Reservar array de cajas
    estado->cajas = (Caja *) malloc(sizeof(Caja) * (size_t)estado->num_cajas);
    if (!estado->cajas) {
        perror("malloc(cajas)");
        return 1;
    }

    // Crear cada caja y asignar mangos dinámicamente
    for (int i = 0; i < estado->num_cajas; i++) {
        estado->cajas[i].id = i + 1;
        estado->cajas[i].area_caja = area_caja;

        // num_mangos con variación hasta 1.2*N
        int extra = (int)(num_mangos_base * 0.2 + 0.9999);
        if (extra < 1) extra = 1;
        estado->cajas[i].num_mangos = num_mangos_base + (rand() % extra);

        estado->cajas[i].mangos = (Mango *) malloc(sizeof(Mango) * (size_t)estado->cajas[i].num_mangos);
        if (!estado->cajas[i].mangos) {
            perror("malloc(mangos)");
            // liberar lo asignado hasta ahora
            for (int j = 0; j < i; j++) free(estado->cajas[j].mangos);
            free(estado->cajas);
            estado->cajas = NULL;
            return 1;
        }
        // Inicializar campos básicos
        for (int j = 0; j < estado->cajas[i].num_mangos; j++) {
            estado->cajas[i].mangos[j].id = j+1;
            estado->cajas[i].mangos[j].etiquetado = 0;
            estado->cajas[i].mangos[j].area = 0.0f;
            estado->cajas[i].mangos[j].x = 0.0f;
            estado->cajas[i].mangos[j].y = 0.0f;
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Calcula cantidad mínima de robots activables (<= robots_maximos) para cubrir N_mangos
// Devuelve M mínimo o -1 si no factible.
// Modelo: robots distribuidos homogéneamente -> distancia entre ejes = longitud_banda / robots_maximos
// Un robot puede operar mientras la caja tarda en recorrer esa distancia.
// -----------------------------------------------------------------------------
int calcular_min_robots_para_rango(EstadoSistema *estado, float area_caja, int N_mangos, int robots_maximos) {
    if (!estado || robots_maximos <= 0) return -1;
    double velocidad_banda = estado->velocidad_banda;
    double longitud_banda = estado->longitud_banda;

    if (velocidad_banda <= 0.0 || longitud_banda <= 0.0 || area_caja <= 0.0 || N_mangos <= 0) {
        fprintf(stderr, "Parametros invalidos para calcular robots.\n");
        return -1;
    }

    // Peor caso: +20%
    int N_max = (int) ceil(N_mangos * 1.2);

    // Lado de la caja y tiempos (segun convención)
    double lado = sqrt((double)area_caja);
    double distancia_diagonal = lado * sqrt(2.0);

    // Interpretación: la velocidad del brazo (cm/s) se modela como v_brazo = lado / CONST_VEL
    double v_brazo = lado / CONST_VEL;
    double T_diagonal = distancia_diagonal / v_brazo; // s
    double T_total = T_diagonal + T_ETIQUETA;        // s por mango

    // Distancia entre ejes si se dispone de robots_maximos en la banda (convención A)
    double distancia_entre_ejes = longitud_banda / (double)robots_maximos;
    // s que un robot tiene "para la caja"

    double cap_por_robot = T_ventana / T_total;
    if (cap_por_robot <= 0) {
        // Un robot no alcanza ni 1 mango en su ventana
        return -1;
    }

    // Buscar mínimo M (1..robots_maximos) tal que M * cap_por_robot >= N_max
    for (int M = 1; M <= robots_maximos; M++) {
        double capacidad_total = cap_por_robot * M;
        // Depuración
        // printf("[DEBUG] M=%d cap_por_robot=%d cap_total=%d N_max=%d\n", M, cap_por_robot, capacidad_total, N_max);
        if (capacidad_total >= N_max) {
            return M;
        }
    }

    // Si no se cumple para M <= robots_maximos, no es factible
    return -1;
}

// -----------------------------------------------------------------------------
// Acomodar mangos en grilla (sin solapamiento): sencillo y determinista
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
            // Área aleatoria entre 70 y 90
            m->area = 70.0f + (float)(rand() % 21);
            float cx = (col + 0.5f) * tamCelda;
            float cy = (fila + 0.5f) * tamCelda;
            // Coordenadas relativas al centro de la caja
            m->x = cx - lado / 2.0f;
            m->y = cy - lado / 2.0f;
            index++;
        }
    }
}

// -----------------------------------------------------------------------------
// Escanear: para cada caja asigna posiciones a los mangos y las imprime
// -----------------------------------------------------------------------------
void escanear(EstadoSistema *estado) {
    if (!estado || !estado->cajas) return;

    printf("\n=== INICIANDO ESCANEO DE CAJAS ===\n");

    for (int i = 0; i < estado->num_cajas; i++) {
        Caja *caja = &estado->cajas[i];
        printf("\nCaja #%d (Área %.2f cm², %d mangos)\n", caja->id, caja->area_caja, caja->num_mangos);

        // Rellenar/marcar IDs (ya hechos en crear_cajas, pero por seguridad)
        for (int j = 0; j < caja->num_mangos; j++) {
            caja->mangos[j].id = j + 1;
            caja->mangos[j].etiquetado = 0;
        }

        acomodarEnGrilla(caja);

        // Mostrar resultado
        for (int j = 0; j < caja->num_mangos; j++) {
            Mango *m = &caja->mangos[j];
            printf(" Mango %2d | área %.1f cm² | pos (%.2f, %.2f)\n", m->id, m->area, m->x, m->y);
        }
    }

    printf("\n=== ESCANEO COMPLETADO ===\n");
}

// -----------------------------------------------------------------------------
// Liberar memoria segura
// -----------------------------------------------------------------------------
void cleanup_estado(EstadoSistema *estado) {
    if (!estado) return;
    if (estado->cajas) {
        for (int i = 0; i < estado->num_cajas; i++) {
            if (estado->cajas[i].mangos) {
                free(estado->cajas[i].mangos);
                estado->cajas[i].mangos = NULL;
            }
        }
        free(estado->cajas);
        estado->cajas = NULL;
    }
}


int enviar_estado(int sock, EstadoSistema *estado, int *robots_maximos) {
    if (!estado) return -1;

    float f;
    int32_t i32;

    // 1) velocidad_banda
    f = (float) estado->velocidad_banda;
    if (send_all(sock, &f, sizeof(f)) < 0) return -1;

    // 2) longitud_banda
    f = (float) estado->longitud_banda;
    if (send_all(sock, &f, sizeof(f)) < 0) return -1;

    // 3) num_robots
    i32 = (int32_t) estado->num_robots;
    if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;

    // 4) num_cajas
    i32 = (int32_t) estado->num_cajas;
    if (send_all(sock, &i32, sizeof(i32)) < 0) return -1;
	
	// 5) robots que hay en la banda
	if(send(sock, robots_maximos, sizeof(int))< 0) return -1;
	
    // 6) por cada caja...
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

    return 0; // OK
}

int send_all(int sock, const void *buffer, size_t length) {
    const char *ptr = buffer;
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = send(sock, ptr + total_sent, length - total_sent, 0);
        if (sent <= 0) return -1;
        total_sent += sent;
    }

    return 0;
}
