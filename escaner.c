#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "datos.h"


#define CONST_VEL 10.0
#define T_ETIQUETA 0.5
#define MAX_ROBOTS 50
#define R_ROBOT 50
#define AREA_MANGO_PROMM 90

int calcular_min_robots_para_rango(EstadoSistema *estado,float area_caja ,int N_mangos, int robots_maximos);
int crear_cajas(EstadoSistema *estado, float area_caja,int robots_maximos);
void acomodarEnGrilla(Caja *caja);
void escanear(EstadoSistema *estado);
void cleanup_estado(EstadoSistema *estado);

int main(int argc,char *argv[]) {
	
	// Variables - Sistema
    EstadoSistema estado = {0};
    float area_caja;
	int robots_maximos; 
	char flag_P = 1;
	
	srand(time(NULL));
	//Variables - Socket
	int server_sockfd, client_sockfd;
	struct sockaddr_in server_address;
	struct sockaddr_in client_address;
	socklen_t server_len, client_len;
	int rc;
	
	server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(server_sockfd < 0){
		perror("Error al crear el socket");
        exit(EXIT_FAILURE);
	}
	
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);
	server_address.sin_port = htons(7734);
    server_len = sizeof(server_address); 
	
	rc = bind(server_sockfd, (struct sockaddr *) &server_address, server_len);
    if(rc < 0){
        perror("Error al asociar el socket a la direccion IP y puerto");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }
    rc = listen(server_sockfd, 5);
    if(rc < 0){
        perror("Error en decirle al sistema que este socket escuchara conexiones entrantes");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }

    client_len = sizeof(client_address);

    //devuelve un nuevo descriptor de socket que sirve para hablar con ese cliente
    client_sockfd = accept(server_sockfd, (struct sockaddr *) &client_address, &client_len);
	if(client_sockfd < 0){
        perror("Error: fallo el accept()");
        close(server_sockfd);
        exit(EXIT_FAILURE);
    }
    
	for(int i = 1; i <argc ; i++){
		if(strcmp(argv[i],"-E") == 0){
			flag_P = 0; 
		}
	}
	
	// Configuración base de la simulación
	if(flag_P == 1){
	    estado.velocidad_banda = 1.0;   // X cm/s
	    estado.longitud_banda = 700.0;  // W cm
		area_caja = 500.0;// cm², tamaño típico de una caja mediana
	    estado.num_cajas = 3;           // cantidad de cajas
	    robots_maximos = 10;//Maximos robots para usar
		
	}else if(flag_P == 0){ //Configuración del usuario para la simulacion
		printf("Elije la velocidad de la banda transportadora: ");
		scanf("%f", &estado.velocidad_banda);
		printf("Elije la longitud de la banda transportadora: ");
		scanf("%f", &estado.longitud_banda);
		printf("Elije el numero de cajas: ");
		scanf("%d", &estado.num_cajas);		
		printf("Elije el area de la caja (cm²): ");
		scanf("%f",&area_caja);
		
	}

	//Crear Cajas
	int result = crear_cajas(&estado, area_caja, robots_maximos);
	if(result == 1){
		perror("Error: No se pudo asignar memoria para los mangos.");
    	close(client_sockfd);
    	close(server_sockfd);
        exit(EXIT_FAILURE);
	}
	
	//Calcular cantidad de robots
	escanear(&estado);
	
	//Prueba de conexion 
	while(1){
		char ch;
		printf("Server connected\n");
		write(client_sockfd, &estado.num_robots, sizeof(int));
		read(client_sockfd, &ch, 1);
		printf("Llego mensaje desde el cliente: %c", ch);
		if(ch == 'X') break;
	}
	
	printf("Server exiting\n");

    close(client_sockfd);
    close(server_sockfd);
	
	cleanup_estado(&estado);
	
	printf("\nSimulación completada correctamente.\n");
	return 0;
}


void cleanup_estado(EstadoSistema *estado){
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

int crear_cajas(EstadoSistema *estado, float area_caja,int robots_maximos){
	
	int num_mangos = (int) ((area_caja / AREA_MANGO_PROMM));
	
	estado->num_robots = calcular_min_robots_para_rango(estado, area_caja, num_mangos, robots_maximos);
	
	estado->cajas = malloc(estado->num_cajas * sizeof(Caja));
	if(estado->cajas == NULL){
		perror("Error: No se pudo asignar memoria para las cajas.");
		exit(1);
	}
	
	for (int i = 0; i < estado->num_cajas; i++) {
	    estado->cajas[i].id = i + 1;
	    estado->cajas[i].area_caja = area_caja;
	    estado->cajas[i].num_mangos = num_mangos + rand() % (int)(num_mangos * 0.2 + 1);  // entre N y a 1.2N mangos
	    estado->cajas[i].mangos = malloc(estado->cajas[i].num_mangos * sizeof(Mango));
	    if (estado->cajas[i].mangos == NULL) {
    		return 1;
		}
	}
	return 0;
}

int calcular_min_robots_para_rango(EstadoSistema *estado,float area_caja ,int N_mangos, int robots_maximos){
	
	
	printf("Velocidad banda: %f\n", estado->velocidad_banda);
	printf("Longitud banda: %f\n", estado->longitud_banda);
	printf("Area de la caja: %f\n", area_caja);
	printf("Numero de mangos: %d\n", N_mangos);
	printf("Numero de robots maximos: %d\n", robots_maximos);
	
	if(estado->velocidad_banda <= 0.0 || estado->longitud_banda <= 0.0 || area_caja <= 0.0 || N_mangos <= 0){
		printf("No cumple condicion");
		return -1;
	}
	int N_max = (int)(N_mangos * 1.2 + 0.9999);
	double lado = sqrt(area_caja);
	double distancia_diagonal = lado * sqrt(2);
	double velocidad_brazoD = lado / CONST_VEL;
	double T_diagonal = distancia_diagonal / velocidad_brazoD;
	double T_total = T_diagonal + T_ETIQUETA;
	
	double distancia_robots = estado->longitud_banda / robots_maximos;
	double T_ventana = distancia_robots / estado->velocidad_banda;  // Tiempo para recorrer toda la banda.
    int capacidad_robot = floor(T_ventana / T_total);
    
    if (capacidad_robot <= 0) {
        return -1;  // Si la capacidad de un solo robot es menor o igual a 0, no se puede etiquetar.
    }

    for (int M = 1; M <= robots_maximos; M++) {
    	
    	int capacidad_total = capacidad_robot * M;
    	double T_entre_robots = T_ventana * M;
        printf("Capacidad total con %d robots: %d mangos\n", M, capacidad_total);
        printf("Numero de mangos maximo promedio: %d\n", N_max);
        printf("Tiempo entre robots 0 y %d: %.2f segundos\n", M, T_entre_robots);
        
        // Verificar si la capacidad total es suficiente para los mangos.
        if (capacidad_total >= N_max) {
            return M;
        }
    }

    printf("No se puede etiquetar todos los mangos con los robots disponibles\n");
    return -1;  // Si no se encontró una cantidad adecuada de robots
}

void acomodarEnGrilla(Caja *caja) {

    float lado = sqrtf(caja->area_caja);

    int N = caja->num_mangos;

    // Número de filas y columnas (cuadrado mínimo)
    int celdas = (int)ceil(sqrtf(N));

    float tamCelda = lado / celdas;

    int index = 0;

    for (int fila = 0; fila < celdas && index < N; fila++) {
        for (int col = 0; col < celdas && index < N; col++) {

            Mango *m = &caja->mangos[index];

            // Área aleatoria
            m->area = 70 + rand() % 21;

            // Centro de la celda
            float cx = (col + 0.5f) * tamCelda;
            float cy = (fila + 0.5f) * tamCelda;

            // Convertir a coordenadas relativas al centro de la caja
            m->x = cx - lado / 2.0f;
            m->y = cy - lado / 2.0f;

            index++;
        }
    }
}


void escanear(EstadoSistema *estado) {

    printf("\nINICIANDO ESCANEO DE CAJAS...\n");

    for (int i = 0; i < estado->num_cajas; i++) {

        Caja *caja = &estado->cajas[i];

        printf("\nCaja #%d (Área %.2f cm², %d mangos)\n",
               caja->id, caja->area_caja, caja->num_mangos);

        // Generar IDs
        for (int j = 0; j < caja->num_mangos; j++)
            caja->mangos[j].id = j+1;

        // Acomodar
        acomodarEnGrilla(caja);

        // Imprimir resultado
        for (int j = 0; j < caja->num_mangos; j++) {

            Mango *m = &caja->mangos[j];

            printf("Mango %2d | área %.1f cm² | pos (%.1f, %.1f)\n",
                   m->id, m->area, m->x, m->y);
        }
    }
}







