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
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "datos.h"
#include "robot.h"

#define DT_SECS 0.05
#define PROB_FALLO 0.1
#define CONST_VEL 10.0
#define T_ETIQUETA 0.5

/* Variables globales (para que los hilos accedan fácilmente al conjunto de robots) */
static RobotInfo *g_robots_infos = NULL;
static int g_robots_maximos = 0;
static SistemaRobot *g_sistema = NULL;

//Metodos traer datos
EstadoSistema *recibir_estado(int sock, int *robots_maximos);
int recv_all(int sock, void *buffer, size_t length);

//Metodos Robot
void inicializar_robots(double T_ventana, SistemaRobot *sistemarobot, int size);
int activar_robot(RobotInfo *robotinfo);
void rutina_robot(void *arg);
int robot_falla(double prob);
void manejar_falla(int id);
void recuperar_robot(int id);

//Metodos Caja
int movercaja(CajaEnBanda *cajaenbanda);
float get_tiempo_caja(CajaEnBanda *cajaenbanda);
int is_caja_activa(CajaEnBanda *cajaenbanda);
void desactivar_caja(CajaEnBanda *cajaenbanda);

int main(){
	
		//EstadoSistema estado;
	    int sockfd;
        struct sockaddr_in client_address;
        int len;
        int result;
        char ch;
        int num_mangos; //QUITARRR
        int robots_maximos;
        EstadoSistema *estado;
        SistemaRobot sistemaRobot;

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

		estado = recibir_estado(sockfd, &robots_maximos);
	
		for (int i = 0; i < estado->num_cajas; i++) {
	        Caja *caja = &estado->cajas[i];
	        printf("\nCaja #%d (Área %.2f cm², %d mangos)\n", caja->id, caja->area_caja, caja->num_mangos);
	        
	        // Mostrar resultado
	        for (int j = 0; j < caja->num_mangos; j++) {
	            Mango *m = &caja->mangos[j];
	            printf(" Mango %2d | área %.1f cm² | pos (%.2f, %.2f)\n", m->id, m->area, m->x, m->y);
	        }
	    }
	    
	    double tiempo_maximo = estado.longitud_banda / estado.velocidad_banda;
	    double T_ventana = tiempo_maximo / robots_maximos;
	    
	    CajaEnBanda *cajas_en_banda = calloc(estado.num_cajas, sizeof(CajaEnBanda));
	    RobotInfo *robots_infos = calloc(robots_maximos, sizeof(RobotInfo));
	    
	    sistemaRobot.robotsinfos = robots_infos;
	    sistemaRobot.cajasenbanda= cajas_en_banda;
	    inicializar_robots(T_ventana, &sistemaRobot, robots_maximos);
	    
	    for(int i = 0; i < estado.num_robots; i++){
	    	int resultado = activar_robot(&robots_infos[i]);
	    	if(resultado == 0){
	    		sistemaRobot.robotsactivos += 1; 
			}
		}
		
		for(int i = 0; i< estado.num_cajas; i++){
			cajas_en_banda[i].caja = &estado.cajas[i];
			cajas_en_banda[i].activa = 1;
			cajas_en_banda[i].tiempo = 0;
			cajas_en_banda[i].lock = 1;
			cajas_en_banda[i].tiempo_max = tiempo_maximo;
			pthread_create(&cajas_en_banda[i].thread, NULL, mover_caja, &cajas_en_banda[i]);
			sleep(ceil(T_ventana));
		}
	    
		
		read(sockfd, &num_mangos, sizeof(int));
		printf("Llego al cliente, numero de robots: %d\n", num_mangos);
		ch = 'X';
		write(sockfd, &ch, 1);
		printf("Cliente terminado");
        close(sockfd);
        exit(0);
}

EstadoSistema *recibir_estado(int sock, int *robots_maximos) {
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

	// 5) guardar robots maximos
    if(recv(sock, robots_maximos, sizeof(int)) < 0) goto fail;

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

void inicializar_robots(double T_ventana, SistemaRobot *sistemarobot, int size) {
	g_robots_infos = sistemarobot->robotsinfos;
    g_robots_maximos = size;
    g_sistema = sistemarobot;   

   
    for(int i = 0; i < size; i++) {
        sistemarobot->robotsinfos[i].id = i;
        sistemarobot->robotsinfos[i].t_start = i * T_ventana;
        sistemarobot->robotsinfos[i].t_end = (i+1) * T_ventana;
        sistemarobot->robotsinfos[i].activo = 0;
        sistemarobot->robotsinfos[i].daniado = 0;
        sistemarobot->robotsinfos[i].es_reemplazo= 0;
        sistemarobot->robotsinfos[i].mangos_etiquetados = 0;
        pthread_mutex_init(&sistemarobot->robotsinfos[i].lock, NULL);
    }
}

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

    printf("Robot %d ACTIVADO (rango %.2f - %.2f)\n",
           robotinfo->id, robotinfo->t_start, robotinfo->t_end);
    return 0;
}

int movercaja(CajaEnBanda *cajaenbanda){
	CajaEnBanda *cajaenbanda = (CajaEnBanda *)arg;

    printf("Caja #%d ingresó a la banda\n", cajaenbanda->caja->id);

    while (1) {
        usleep((useconds_t)(DT_SECS * 1e6));

        pthread_mutex_lock(&cajaenbanda->lock);

        if (!cajaenbanda->activa) {
            pthread_mutex_unlock(&cajaenbanda->lock);
            return NULL;
        }

        cajaenbanda->tiempo += DT_SECS;

        if (cajaenbanda->tiempo >= cajaenbanda->tiempo_max) {
            cajaenbanda->activa = 0;
            pthread_mutex_unlock(&cajaenbanda->lock);
            printf("Caja #%d salió de la banda\n", cajaenbanda->caja->id);
            return NULL;
        }

        pthread_mutex_unlock(&cajaenbanda->lock);
    }
}

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
    pthread_mutex_lock(cajaenbanda->lock);
    cajaenbanda->activa = 0;
    pthread_mutex_unlock(cajaenbanda->lock);
}

void rutina_robot(void *arg){
	RobotInfo *r = (RobotInfo *)arg;
    if (!r) return NULL;

    /* Seed RNG por hilo para rand() */
    unsigned int seed = (unsigned int) (time(NULL) ^ (r->id * 7919));
    srand(seed);

    printf("Hilo robot %d iniciado.\n", r->id);

    while (1) {
        /* Si el robot fue marcado inactivo (por ejemplo: se decidió apagar), salimos */
        pthread_mutex_lock(&r->lock);
        int es_activo = r->activo;
        int esta_daniado = r->daniado;
        pthread_mutex_unlock(&r->lock);

        if (!es_activo && !r->es_reemplazo) {
            /* Si no está activo y no es reemplazo, terminamos el hilo limpiamente */
            printf("Robot %d finaliza su hilo (inactivo, no reemplazo).\n", r->id);
            break;
        }

        /* Si el robot está dañado, esperamos a que sea recuperado (handle_recovery) */
        if (esta_daniado) {
            /* Dormimos un tiempo corto y chequeamos de nuevo; recovery se maneja internamente */
            sleep(1);
            continue;
        }

        /* Simular trabajo: aquí podrías intentar etiquetar mangos (si quisieras).
           Por simplicidad hacemos una cuenta/espera y posibilidad de fallo. */
        usleep((useconds_t)(DT_SECS * 1e6)); /* paso de simulación */

        /* Probabilidad de fallo */
        if (robot_falla(PROB_FALLO)) {
            printf("Robot %d: ocurrio un fallo!\n", r->id);
            manejar_falla(r->id);
            /* después de marcar fallo, el propio hilo seguirá en bucle y detectará 'daniado' */
            continue;
        }

        /* Si llegamos aquí, el robot "trabajó" exitosamente en este tick.
           (Puedes incrementar estadística simple) */
        pthread_mutex_lock(&r->lock);
        r->mangos_etiquetados += 0; /* queda como marcador; implementar conteo real si quieres */
        pthread_mutex_unlock(&r->lock);
    }

    return NULL;

}

int robot_falla(double prob) {
    double r = (double) rand() / RAND_MAX;
    return r < prob;
}

void manejar_falla(int id) {
    if (!g_robots_infos || id < 0 || id >= g_robots_maximos) return;

    /* marcar como dañado */
    pthread_mutex_lock(&g_robots_infos[id].lock);
    g_robots_infos[id].daniado = 1;
    g_robots_infos[id].activo = 0;
    pthread_mutex_unlock(&g_robots_infos[id].lock);

    /* buscar reemplazo */
    int found = -1;
    for (int i = 0; i < g_robots_maximos; i++) {
        if (i == id) continue;
        pthread_mutex_lock(&g_robots_infos[i].lock);
        int candidate_free = (!g_robots_infos[i].activo && !g_robots_infos[i].daniado && !g_robots_infos[i].es_reemplazo);
        if (candidate_free) {
            g_robots_infos[i].activo = 1;
            g_robots_infos[i].es_reemplazo = 1;
            pthread_mutex_unlock(&g_robots_infos[i].lock);
            found = i;
            break;
        }
        pthread_mutex_unlock(&g_robots_infos[i].lock);
    }

    if (found >= 0) {
        printf("Robot %d se daño -> Robot %d activado como reemplazo\n", id, found);
    } else {
        printf("Robot %d se daño -> NO HAY reemplazo disponible\n", id);
    }

    /* Simular reparación autónoma después de un tiempo aleatorio (por ejemplo 1..5 s)
       Para mantenerlo simple usamos sleep en una pequeña rutina dentro de este hilo del manejador. */
    int repair_time = 1 + (rand() % 5); /* segundos */
    sleep((unsigned)repair_time);

    /* Ejecutar recuperación */
    recuperar_robot(id);
}

void recuperar_robot(int id) {
    if (!g_robots_infos || id < 0 || id >= g_robots_maximos) return;

    pthread_mutex_lock(&g_robots_infos[id].lock);
    g_robots_infos[id].daniado = 0;
    g_robots_infos[id].activo = 1; /* volver a poner activo */
    pthread_mutex_unlock(&g_robots_infos[id].lock);

    /* Buscar un robot que esté marcado como reemplazo y apagarlo (regla simple: el primero) */
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




