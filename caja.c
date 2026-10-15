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
#include <ctype.h>
#include <math.h>

#define MAX_SOLICITUD 256
#define MAX_TAREAS    100

typedef struct {
    char   texto[MAX_SOLICITUD];
    double duracion;     
    int    completada;   
    int    meeseeks;     
} Tarea;

static Tarea bitacora[MAX_TAREAS];
static int   total = 0;

static void informe(void) {
    int hechas = 0;

    printf("\n================ INFORME DEL BOX ================\n");
    printf("%-3s %-30s %12s %-10s %8s\n", "#", "Solicitud", "Duracion(s)", "Estado", "Meeseeks");
    for (int k = 0; k < total; k++) {
        printf("%-3d %-30.30s %12.6f %-10s %8d\n", k + 1, bitacora[k].texto,
               bitacora[k].duracion,
               bitacora[k].completada ? "Terminada" : "Fallida",
               bitacora[k].meeseeks);
        hechas += bitacora[k].completada;
    }
    printf("-------------------------------------------------\n");
    printf("Tareas solicitadas: %d | Terminadas: %d | No terminadas: %d\n",
           total, hechas, total - hechas);
}

static double ahora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *cur;        
static const char *calc_error; 

static double e_or(void);

static void saltar_espacios(void) {
    while (isspace((unsigned char)*cur)) cur++;
}

static int viene(const char *op) {
    saltar_espacios();
    size_t n = strlen(op);
    if (strncmp(cur, op, n) == 0) {
        cur += n;
        return 1;
    }
    return 0;
}

static void fallo(const char *msg) {
    if (!calc_error) calc_error = msg;   
}

static double e_primario(void) {
    saltar_espacios();
    if (viene("(")) {
        double v = e_or();
        if (!viene(")")) fallo("falta cerrar parentesis");
        return v;
    }
    if (isdigit((unsigned char)*cur) || *cur == '.') {
        char *fin;
        double v = strtod(cur, &fin);
        cur = fin;
        return v;
    }
    fallo(*cur ? "simbolo inesperado" : "expresion incompleta");
    return 0;
}

static double e_unario(void) {
    if (viene("-")) return -e_unario();
    if (viene("+")) return e_unario();
    saltar_espacios();
    if (*cur == '!' && cur[1] != '=') {   
        cur++;
        return e_unario() == 0;
    }
    return e_primario();
}

static double e_mult(void) {
    double v = e_unario();
    for (;;) {
        if (viene("*")) {
            v *= e_unario();
        } else if (viene("/")) {
            double d = e_unario();
            if (d == 0) { fallo("division entre cero"); return 0; }
            v /= d;
        } else if (viene("%")) {
            double d = e_unario();
            if (d == 0) { fallo("modulo entre cero"); return 0; }
            v = fmod(v, d);
        } else {
            return v;
        }
    }
}

static double e_suma(void) {
    double v = e_mult();
    for (;;) {
        if (viene("+"))      v += e_mult();
        else if (viene("-")) v -= e_mult();
        else return v;
    }
}

static double e_cmp(void) {
    double v = e_suma();
    for (;;) {   
        if (viene("=="))      v = (v == e_suma());
        else if (viene("!=")) v = (v != e_suma());
        else if (viene("<=")) v = (v <= e_suma());
        else if (viene(">=")) v = (v >= e_suma());
        else if (viene("<"))  v = (v <  e_suma());
        else if (viene(">"))  v = (v >  e_suma());
        else return v;
    }
}

static double e_and(void) {
    double v = e_cmp();
    while (viene("&&")) {
        double d = e_cmp();
        v = (v != 0 && d != 0);
    }
    return v;
}

static double e_or(void) {
    double v = e_and();
    while (viene("||")) {
        double d = e_and();
        v = (v != 0 || d != 0);
    }
    return v;
}

static int calcular(const char *expresion, double *resultado, const char **error) {
    cur = expresion;
    calc_error = NULL;

    double v = e_or();
    saltar_espacios();
    if (!calc_error && *cur != '\0') fallo("simbolo inesperado");

    if (calc_error) {
        *error = calc_error;
        return 0;
    }
    *resultado = v;
    return 1;
}

static int ejecutar_programa(const char *comando) {
    fflush(stdout);
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", comando, (char *)NULL);
        perror("execl");   
        _exit(127);
    }

    int estado;
    waitpid(pid, &estado, 0);

    if (WIFEXITED(estado)) {
        int codigo = WEXITSTATUS(estado);
        printf("[pid %d, ppid %d] El programa termino con codigo %d\n",
               getpid(), getppid(), codigo);
        return codigo == 0;
    }
    if (WIFSIGNALED(estado)) {
        printf("[pid %d, ppid %d] El programa murio por la senal %d\n",
               getpid(), getppid(), WTERMSIG(estado));
    }
    return 0;
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

    int ok;
    if (strncmp(solicitud, "run ", 4) == 0) {
        ok = ejecutar_programa(solicitud + 4);      
    } else if (strncmp(solicitud, "calc ", 5) == 0) {
        double valor;
        const char *error;
        ok = calcular(solicitud + 5, &valor, &error);      /* parte B */
        if (ok)
            printf("[pid %d, ppid %d] Resultado: %g\n", getpid(), getppid(), valor);
        else
            printf("[pid %d, ppid %d] Error en el calculo: %s\n", getpid(), getppid(), error);
    } else {
        ok = 1;                                     
    }

    if (ok)
        printf("[pid %d, ppid %d] All done! Solicitud completada. Fue un placer servirle.\n",
               getpid(), getppid());
    else
        printf("[pid %d, ppid %d] Ooh, no pude completar la solicitud.\n",
               getpid(), getppid());
    printf("[pid %d, ppid %d] Adios! *poof*\n", getpid(), getppid());
    fflush(stdout);
    exit(ok ? 0 : 1); /* 0 = tarea completada */
}

int main(void) {
    char solicitud[MAX_SOLICITUD];

    printf("=== Box Mr. Meeseeks === (escriba \"adios\" para salir)\n");

    while (1) {
        printf("Box> ");
        fflush(stdout);

        if (fgets(solicitud, sizeof solicitud, stdin) == NULL) break; 
        solicitud[strcspn(solicitud, "\n")] = '\0';                   

        if (solicitud[0] == '\0') continue;
        if (strcmp(solicitud, "adios") == 0) break;

        if (total == MAX_TAREAS) {
            printf("[Box %d] Bitacora llena, no acepto mas solicitudes\n", getpid());
            break;
        }
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

        /* anota la tarea en la bitacora */
        Tarea *t = &bitacora[total++];
        strcpy(t->texto, solicitud);
        t->duracion   = t1 - t0;
        t->completada = ok;
        t->meeseeks   = 1;   

        printf("[Box %d] Solicitud %d %s en %.6f s\n",
               getpid(), total, ok ? "completada" : "NO completada", t->duracion);
    }

    informe();
    printf("[Box %d] Adios!\n", getpid());
    return 0;
}