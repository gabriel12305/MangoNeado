#ifndef ROBOT_H
#define ROBOT_H

typedef struct {
    Caja *caja;
    float tiempo;  // segundos desde que entra a la banda
	int tiempo_max;
    int activa;    // 1 = esta en la banda
    pthread_mutex_t lock;
    pthread_t thread;
} CajaEnBanda;

typedef struct {
    int id;                 // Indice fisico 0..ROBOTS_MAX-1
    double t_start;         // inicio de ventana (s)
    double t_end;           // fin de ventana (s)
	int activo;
	int daniado;
	int es_reemplazo;
    pthread_t thread;       // hilo asociado (si se crea)
    pthread_mutex_t lock;   // mutex para campos del robot
    int mangos_etiquetados; // estadistica
} RobotInfo;

typedef struct {
	int robotsactivos;
	RobotInfo *robotsinfos;
	CajaEnBanda *cajasenbanda;
}SistemaRobot;

#endif
