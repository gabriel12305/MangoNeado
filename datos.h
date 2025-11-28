#ifndef DATOS_H
#define DATOS_H

// ---------- ESTRUCTURAS DE DATOS PRINCIPALES ----------

// Representa un mango individual
typedef struct {
    int id;              // ID único del mango
    float x;             // Posición X relativa dentro de la caja (cm)
    float y;             // Posición Y relativa dentro de la caja (cm)
    float area;          // Área del mango (cm²)
    int etiquetado;      // 0 = no etiquetado, 1 = etiquetado
} Mango;

// Representa una caja sobre la banda
typedef struct {
    int id;              // ID de la caja
    float area_caja;     // Área de la caja (cm²)
    int num_mangos;      // Número de mangos dentro de la caja
    Mango *mangos;       // Arreglo dinámico de mangos
} Caja;

// Representa el estado general del sistema
typedef struct {
    float velocidad_banda;   // Velocidad lineal de la banda (cm/s)
    float longitud_banda;    // Longitud total de la banda (cm)
    int num_robots;          // Número de robots en operación
    int num_cajas;           // Número total de cajas en la simulación
    Caja *cajas;             // Arreglo dinámico de cajas
} EstadoSistema;

#endif

