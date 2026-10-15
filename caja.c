/*
 * Instituto Tecnologico de Costa Rica
 * Campus Central Cartago
 * Ingenieria en Computacion
 * IC-6600 Principios de Sistemas Operativos 
 * SO - Tarea 3: Box Mr. Meeseeks
 * Estudiante: Emily Sanchez Orozco - 2021067314
 * Prof. Esteban Arias Mendez
 * II Semestre 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_SOLICITUD 256

static double ahora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void meeseeks(int fd_lectura, int N, int i) {
    char solicitud[MAX_SOLICITUD];

    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), N, i);

    ssize_t n = read(fd_lectura, solicitud, sizeof solicitud - 1);
    close(fd_lectura);
    if (n <= 0) {
        printf("[pid %d, ppid %d] No recibi ninguna solicitud\n", getpid(), getppid());
        exit(1);
    }
    solicitud[n] = '\0';

    printf("[pid %d, ppid %d] Recibi por pipe: \"%s\"\n",
           getpid(), getppid(), solicitud);
    fflush(stdout);
    exit(0); /* 0 = tarea completada */
}

int main(void) {
    char solicitud[MAX_SOLICITUD];
    int total = 0;

    printf("=== Box Mr. Meeseeks === (escriba \"adios\" para salir)\n");

    while (1) {
        printf("Box> ");
        fflush(stdout);

        if (fgets(solicitud, sizeof solicitud, stdin) == NULL) break; 
        solicitud[strcspn(solicitud, "\n")] = '\0';                   

        if (solicitud[0] == '\0') continue;
        if (strcmp(solicitud, "adios") == 0) break;

        total++;
        double t0 = ahora();

        int fd[2];               
        if (pipe(fd) < 0) {
            perror("pipe");
            continue;
        }

        fflush(stdout);          
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            close(fd[0]);
            close(fd[1]);
            continue;
        }
        if (pid == 0) {
            close(fd[1]);              
            meeseeks(fd[0], 1, 1);     
        }

        /* padre, solo escribe */
        close(fd[0]);
        if (write(fd[1], solicitud, strlen(solicitud)) < 0) perror("write");
        close(fd[1]);                 

        /* espera a que el Meeseeks termine */
        int estado;
        waitpid(pid, &estado, 0);
        double t1 = ahora();

        int ok = WIFEXITED(estado) && WEXITSTATUS(estado) == 0;
        printf("[Box %d] Solicitud %d %s en %.6f s\n",
               getpid(), total, ok ? "completada" : "NO completada", t1 - t0);
    }

    printf("[Box %d] Adios. Solicitudes recibidas: %d\n", getpid(), total);
    return 0;
}