// robot.c  - Cliente "robots" para MangoNeado
// Compilar: gcc robot.c -o robot -lpthread -lm

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include "datos.h"
#include "robot.h"

#define DT_SECS 0.05
#define PROB_FALLO 0.1   /* B por segundo; puedes reemplazar por la variable que envie el servidor si quieres */
#define CONST_VEL 10.0
#define T_ETIQUETA 0.5

/* Globals para que los hilos los encuentren fácilmente */
static RobotInfo *g_robots_infos = NULL;
static int g_robots_maximos = 0;
static SistemaRobot *g_sistema = NULL;
static int g_num_cajas = 0;

/* Prototipos */
EstadoSistema *recibir_estado(int sock, int *robots_maximos);
int recv_all(int sock, void *buffer, size_t length);

/* Robot/caja */
void inicializar_robots(double T_ventana, SistemaRobot *sistemarobot, int size, int num_cajas);
int activar_robot(RobotInfo *robotinfo);
void *rutina_robot(void *arg);

/* Caja en banda */
void *mover_caja(void *arg);
float get_tiempo_caja(CajaEnBanda *cajaenbanda);
int is_caja_activa(CajaEnBanda *cajaenbanda);
void desactivar_caja(CajaEnBanda *cajaenbanda);

/* Fallas / redundancia */
int robot_falla_tick(double prob_per_s);
void manejar_falla(int id);
void recuperar_robot(int id);

/* Util */
static double distancia_2d(double x1, double y1, double x2, double y2);

/* ---------------------------- MAIN ---------------------------- */
int main(){
    int sockfd;
    struct sockaddr_in client_address;
    int len;
    int result;
    char ch;
    int robots_maximos;
    EstadoSistema *estado;
    SistemaRobot sistemaRobot;

    /* crear socket y conectar al escaner (servidor) */
    sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket()");
        exit(EXIT_FAILURE);
    }

    client_address.sin_family = AF_INET;
    client_address.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_address.sin_port = htons(7734);
    len = sizeof(client_address);

    result = connect(sockfd, (struct sockaddr *) &client_address, len);
    if (result < 0) {
        perror("connect()");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    estado = recibir_estado(sockfd, &robots_maximos);
    if (!estado) {
        fprintf(stderr, "Error recibiendo estado del servidor\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    /* Mostrar lo recibido */
    for (int i = 0; i < estado->num_cajas; i++) {
        Caja *caja = &estado->cajas[i];
        printf("\nCaja #%d (Área %.2f cm², %d mangos)\n", caja->id, caja->area_caja, caja->num_mangos);
        for (int j = 0; j < caja->num_mangos; j++) {
            Mango *m = &caja->mangos[j];
            printf(" Mango %2d | área %.1f cm² | pos (%.2f, %.2f) | etiqu: %d\n",
                   m->id, m->area, m->x, m->y, m->etiquetado);
        }
    }

    double tiempo_maximo = estado->longitud_banda / estado->velocidad_banda;
    double T_ventana = tiempo_maximo / (double)robots_maximos;

    /* reservar estructuras locales */
    CajaEnBanda *cajas_en_banda = calloc(estado->num_cajas, sizeof(CajaEnBanda));
    RobotInfo *robots_infos = calloc(robots_maximos, sizeof(RobotInfo));
    if (!cajas_en_banda || !robots_infos) {
        perror("calloc");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    sistemaRobot.robotsinfos = robots_infos;
    sistemaRobot.cajasenbanda = cajas_en_banda;
    sistemaRobot.robotsactivos = 0;

    /* inicializar robots y cajas */
    g_num_cajas = estado->num_cajas;
    inicializar_robots(T_ventana, &sistemaRobot, robots_maximos, estado->num_cajas);

    /* activar los num_robots que saca el escaner (estado->num_robots) */
    for (int i = 0; i < estado->num_robots && i < robots_maximos; i++) {
        if (activar_robot(&robots_infos[i]) == 0) {
            sistemaRobot.robotsactivos += 1;
        }
    }

    /* inicializar cajas en banda y lanzar un hilo por caja */
    for (int i = 0; i < estado->num_cajas; i++) {
        cajas_en_banda[i].caja = &estado->cajas[i];
        cajas_en_banda[i].activa = 1;
        cajas_en_banda[i].tiempo = 0.0f;
        cajas_en_banda[i].tiempo_max = (int)ceil(tiempo_maximo);
        pthread_mutex_init(&cajas_en_banda[i].lock, NULL);
        if (pthread_create(&cajas_en_banda[i].thread, NULL, mover_caja, &cajas_en_banda[i]) != 0) {
            perror("pthread_create(mover_caja)");
        }
        /* esperar un poco entre cajas para que no choquen en rango (como tú hacías) */
        sleep((unsigned int)ceil(T_ventana));
    }

    /* Esperar señal del servidor (como en tu código) */
    read(sockfd, &robots_maximos, sizeof(int));
    printf("Cliente: lectura adicional del servidor OK, valor leido: %d\n", robots_maximos);
    ch = 'X';
    write(sockfd, &ch, 1);

    /* Esperar que los hilos terminen (opcional) */
    for (int i = 0; i < estado->num_cajas; i++) {
        pthread_join(cajas_en_banda[i].thread, NULL);
    }
    /* indicar a robots que finalicen (si quieres) */
    for (int i = 0; i < g_robots_maximos; i++) {
        pthread_mutex_lock(&robots_infos[i].lock);
        robots_infos[i].activo = 0;
        robots_infos[i].es_reemplazo = 0;
        pthread_mutex_unlock(&robots_infos[i].lock);
    }
    for (int i = 0; i < g_robots_maximos; i++) {
        if (robots_infos[i].thread) pthread_join(robots_infos[i].thread, NULL);
    }

    /* limpieza */
    for (int i = 0; i < estado->num_cajas; i++) {
        free(estado->cajas[i].mangos);
    }
    free(estado->cajas);
    free(estado);
    free(cajas_en_banda);
    free(robots_infos);

    close(sockfd);
    return 0;
}

/* ------------------ recibir_estado / recv_all ------------------ */
EstadoSistema *recibir_estado(int sock, int *robots_maximos) {
    float f;
    int32_t i32;

    EstadoSistema *estado = malloc(sizeof(EstadoSistema));
    if (!estado) return NULL;
    memset(estado, 0, sizeof(*estado));

    if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
    estado->velocidad_banda = (double) f;

    if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
    estado->longitud_banda = (double) f;

    if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
    estado->num_robots = (int) i32;

    if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
    estado->num_cajas = (int) i32;

    /* robots_maximos enviados por servidor */
    if (recv(sock, robots_maximos, sizeof(int), 0) <= 0) goto fail;

    if (estado->num_cajas <= 0) goto fail;
    estado->cajas = malloc(sizeof(Caja) * (size_t)estado->num_cajas);
    if (!estado->cajas) goto fail;

    for (int c = 0; c < estado->num_cajas; ++c) {
        Caja *caja = &estado->cajas[c];
        memset(caja, 0, sizeof(*caja));

        if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
        caja->id = (int) i32;

        if (recv_all(sock, &f, sizeof(f)) < 0) goto fail;
        caja->area_caja = f;

        if (recv_all(sock, &i32, sizeof(i32)) < 0) goto fail;
        caja->num_mangos = (int) i32;

        if (caja->num_mangos < 0) goto fail;
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
    if (estado) {
        if (estado->cajas) {
            for (int j = 0; j < estado->num_cajas; ++j) {
                if (estado->cajas[j].mangos) free(estado->cajas[j].mangos);
            }
            free(estado->cajas);
        }
        free(estado);
    }
    return NULL;
}

int recv_all(int sock, void *buffer, size_t length) {
    char *ptr = buffer;
    size_t total_received = 0;
    while (total_received < length) {
        ssize_t rec = recv(sock, ptr + total_received, length - total_received, 0);
        if (rec <= 0) return -1;
        total_received += (size_t)rec;
    }
    return 0;
}

/* ------------------ inicializar_robots ------------------ */
void inicializar_robots(double T_ventana, SistemaRobot *sistemarobot, int size, int num_cajas) {
    g_robots_infos = sistemarobot->robotsinfos;
    g_robots_maximos = size;
    g_sistema = sistemarobot;
    g_num_cajas = num_cajas;

    for (int i = 0; i < size; i++) {
        sistemarobot->robotsinfos[i].id = i;
        /* ventanas uniformes a lo largo de la banda (0..T_total) */
        sistemarobot->robotsinfos[i].t_start = i * T_ventana;
        sistemarobot->robotsinfos[i].t_end = (i + 1) * T_ventana;
        sistemarobot->robotsinfos[i].activo = 0;
        sistemarobot->robotsinfos[i].daniado = 0;
        sistemarobot->robotsinfos[i].es_reemplazo = 0;
        sistemarobot->robotsinfos[i].mangos_etiquetados = 0;
        pthread_mutex_init(&sistemarobot->robotsinfos[i].lock, NULL);
    }
}

/* ------------------ activar_robot ------------------ */
int activar_robot(RobotInfo *robotinfo) {
    if (!robotinfo) return -1;
    pthread_mutex_lock(&robotinfo->lock);
    if (robotinfo->activo || robotinfo->daniado) {
        pthread_mutex_unlock(&robotinfo->lock);
        return -1;
    }
    robotinfo->activo = 1;
    pthread_mutex_unlock(&robotinfo->lock);

    if (pthread_create(&robotinfo->thread, NULL, rutina_robot, robotinfo) != 0) {
        perror("pthread_create(rutina_robot)");
        pthread_mutex_lock(&robotinfo->lock);
        robotinfo->activo = 0;
        pthread_mutex_unlock(&robotinfo->lock);
        return -1;
    }
    printf("Robot %d ACTIVADO (rango %.2f - %.2f)\n", robotinfo->id, robotinfo->t_start, robotinfo->t_end);
    return 0;
}

/* ------------------ mover_caja (hilo) ------------------ */
void *mover_caja(void *arg) {
    CajaEnBanda *c = (CajaEnBanda *)arg;
    printf("Caja #%d entró a la banda (tiempo_max %.2f s)\n", c->caja->id, (double)c->tiempo_max);
    while (1) {
        usleep((useconds_t)(DT_SECS * 1e6));
        pthread_mutex_lock(&c->lock);
        if (!c->activa) {
            pthread_mutex_unlock(&c->lock);
            return NULL;
        }
        c->tiempo += DT_SECS;
        if (c->tiempo >= (float)c->tiempo_max) {
            c->activa = 0;
            pthread_mutex_unlock(&c->lock);
            printf("Caja #%d salió de la banda (tiempo >= tiempo_max)\n", c->caja->id);
            return NULL;
        }
        pthread_mutex_unlock(&c->lock);
    }
}

/* ------------------ auxiliares caja ------------------ */
float get_tiempo_caja(CajaEnBanda *cajaenbanda) {
    if (!cajaenbanda) return 0.0f;
    float t;
    pthread_mutex_lock(&cajaenbanda->lock);
    t = cajaenbanda->tiempo;
    pthread_mutex_unlock(&cajaenbanda->lock);
    return t;
}

int is_caja_activa(CajaEnBanda *cajaenbanda) {
    if (!cajaenbanda) return 0;
    int a;
    pthread_mutex_lock(&cajaenbanda->lock);
    a = cajaenbanda->activa;
    pthread_mutex_unlock(&cajaenbanda->lock);
    return a;
}

void desactivar_caja(CajaEnBanda *cajaenbanda) {
    if (!cajaenbanda) return;
    pthread_mutex_lock(&cajaenbanda->lock);
    cajaenbanda->activa = 0;
    pthread_mutex_unlock(&cajaenbanda->lock);
}

/* ------------------ rutina_robot con etiquetado real ------------------ */
void *rutina_robot(void *arg) {
    RobotInfo *r = (RobotInfo *)arg;
    if (!r) return NULL;

    /* usar rand_r para hilos */
    unsigned int seed = (unsigned int)time(NULL) ^ (r->id * 1315423911u);

    printf("Hilo robot %d iniciado (ventana %.2f - %.2f)\n", r->id, r->t_start, r->t_end);

    /* posición del brazo en la caja (comienza en centro) */
    double arm_x = 0.0;
    double arm_y = 0.0;

    while (1) {
        pthread_mutex_lock(&r->lock);
        int activo = r->activo;
        int daniado = r->daniado;
        int es_reemp = r->es_reemplazo;
        pthread_mutex_unlock(&r->lock);

        /* si fue desactivado completamente -> salir */
        if (!activo && !es_reemp) {
            printf("Robot %d: saliendo (inactivo y no reemplazo)\n", r->id);
            break;
        }

        if (daniado) {
            /* esperar reparación (se maneja en manejar_falla) */
            sleep(1);
            continue;
        }

        /* recorrer cajas y buscar una dentro del rango */
        int trabajo = 0;
        for (int ci = 0; ci < g_num_cajas; ci++) {
            CajaEnBanda *cb = &g_sistema->cajasenbanda[ci];
            if (!is_caja_activa(cb)) continue;

            float t_caja = get_tiempo_caja(cb);
            /* si la caja no está en la ventana del robot, seguir */
            if (t_caja < r->t_start || t_caja >= r->t_end) continue;

            /* tenemos una caja en rango; trabajamos en ella (solo una caja a la vez) */
            /* buscamos el mango NO etiquetado más cercano a la posición actual del brazo */
            int best_idx = -1;
            double best_dist = 1e9;

            pthread_mutex_lock(&cb->lock);
            for (int mi = 0; mi < cb->caja->num_mangos; mi++) {
                Mango *m = &cb->caja->mangos[mi];
                if (m->etiquetado) continue;
                double d = distancia_2d(arm_x, arm_y, (double)m->x, (double)m->y);
                if (d < best_dist) {
                    best_dist = d;
                    best_idx = mi;
                }
            }

            if (best_idx >= 0) {
                Mango *m = &cb->caja->mangos[best_idx];

                /* calcular tiempos */
                double lado = sqrt((double)cb->caja->area_caja);
                double v_brazo = lado / CONST_VEL; /* cm/s */
                if (v_brazo <= 0.0) v_brazo = 1.0;
                double t_move = best_dist / v_brazo;
                double t_total = t_move + T_ETIQUETA;

                /* Antes de "mover", chequear prob de falla en este tick */
                double p_tick = PROB_FALLO * DT_SECS;
                double rrand = (double)rand_r(&seed) / (double)RAND_MAX;
                if (rrand < p_tick) {
                    pthread_mutex_unlock(&cb->lock);
                    printf("Robot %d: fallo antes de mover al mango (simulado)\n", r->id);
                    manejar_falla(r->id);
                    trabajo = 1;
                    break; /* salimos a loop principal para ver estado de falla */
                }

                /* Simular movimiento+etiquetado (bloqueante) */
                pthread_mutex_unlock(&cb->lock);

                /* Sleep por el tiempo de movimiento + etiqueta (simula acción) */
                usleep((useconds_t)(t_total * 1e6));

                /* Después de dormir, bloquear la caja y verificar que el mango no haya sido
                   etiquetado por otra acción (en principio no debería, pero por seguridad) */
                pthread_mutex_lock(&cb->lock);
                if (!m->etiquetado) {
                    m->etiquetado = 1;
                    r->mangos_etiquetados += 1;
                    /* actualizar arm position al mango etiquetado */
                    arm_x = m->x;
                    arm_y = m->y;
                    printf("Robot %d: etiquetó mango %d en caja %d (pos %.2f, %.2f). Tiempo usado %.2fs\n",
                           r->id, m->id, cb->caja->id, m->x, m->y, t_total);
                } else {
                    /* ya etiquetado por otra (caso raro) */
                    arm_x = m->x;
                    arm_y = m->y;
                }
                pthread_mutex_unlock(&cb->lock);

                trabajo = 1;
                /* una vez etiquetamos 1 mango, volvemos a buscar (permite fairness) */
            } else {
                pthread_mutex_unlock(&cb->lock);
            }

            if (trabajo) break;
        } /* fin for cajas */

        /* si no trabajó en ninguna caja, dormir un tick corto */
        if (!trabajo) usleep((useconds_t)(DT_SECS * 1e6));
    } /* fin while */

    return NULL;
}

/* ------------------ falla / redundancia ------------------ */

/* Probabilidad de fallo se calcula como prob_por_segundo * DT_SECS por tick
   (aplicado dentro de rutina_robot). Aqui solo funcion auxiliares. */
int robot_falla_tick(double prob_per_sec) {
    double p = prob_per_sec * DT_SECS;
    if (p <= 0.0) return 0;
    if (p > 1.0) p = 1.0;
    double r = (double)rand() / RAND_MAX;
    return r < p;
}

/* Manejar falla: marcar daniado, desactivar, activar primer reemplazo libre */
void manejar_falla(int id) {
    if (!g_robots_infos || id < 0 || id >= g_robots_maximos) return;

    pthread_mutex_lock(&g_robots_infos[id].lock);
    g_robots_infos[id].daniado = 1;
    g_robots_infos[id].activo = 0;
    pthread_mutex_unlock(&g_robots_infos[id].lock);

    int found = -1;
    for (int i = 0; i < g_robots_maximos; i++) {
        if (i == id) continue;
        pthread_mutex_lock(&g_robots_infos[i].lock);
        int candidate_free = (!g_robots_infos[i].activo && !g_robots_infos[i].daniado && !g_robots_infos[i].es_reemplazo);
        if (candidate_free) {
            g_robots_infos[i].es_reemplazo = 1;
            /* activar este robot */
            g_robots_infos[i].activo = 1;
            pthread_mutex_unlock(&g_robots_infos[i].lock);
            /* crear hilo */
            if (pthread_create(&g_robots_infos[i].thread, NULL, rutina_robot, &g_robots_infos[i]) != 0) {
                perror("pthread_create(reemplazo)");
            }
            found = i;
            break;
        }
        pthread_mutex_unlock(&g_robots_infos[i].lock);
    }

    if (found >= 0) {
        printf("Robot %d se dañó -> Robot %d activado como reemplazo\n", id, found);
    } else {
        printf("Robot %d se dañó -> NO HAY reemplazo disponible\n", id);
    }

    /* Simular reparación después de un tiempo aleatorio 1..5 s */
    int repair_time = 1 + (rand() % 5);
    sleep((unsigned)repair_time);

    /* recuperar */
    recuperar_robot(id);
}

/* Recuperar robot original y apagar reemplazo (el primero que encontremos) */
void recuperar_robot(int id) {
    if (!g_robots_infos || id < 0 || id >= g_robots_maximos) return;

    pthread_mutex_lock(&g_robots_infos[id].lock);
    g_robots_infos[id].daniado = 0;
    g_robots_infos[id].activo = 1;
    pthread_mutex_unlock(&g_robots_infos[id].lock);

    for (int i = 0; i < g_robots_maximos; i++) {
        if (i == id) continue;
        pthread_mutex_lock(&g_robots_infos[i].lock);
        if (g_robots_infos[i].es_reemplazo) {
            g_robots_infos[i].es_reemplazo = 0;
            g_robots_infos[i].activo = 0;
            pthread_mutex_unlock(&g_robots_infos[i].lock);
            printf("Robot %d recuperado -> Robot %d (reemplazo) DESACTIVADO\n", id, i);
            return;
        }
        pthread_mutex_unlock(&g_robots_infos[i].lock);
    }
    printf("Robot %d recuperado -> No habia reemplazo activo.\n", id);
}

/* ------------------ util ------------------ */
static double distancia_2d(double x1, double y1, double x2, double y2) {
    double dx = x1 - x2;
    double dy = y1 - y2;
    return sqrt(dx*dx + dy*dy);
}

