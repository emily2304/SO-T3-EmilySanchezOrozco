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
#include <sched.h>
#include <semaphore.h>
#include <sys/mman.h>

#define MAX_SOLICITUD 256
#define MAX_TAREAS    100
#define AL_AZAR       (-1.0)   /* valor que significa "sortealo tu, Meeseeks" */
#define MAX_HIJOS     5
#define ANEJAMIENTO   1.20     /* cada nivel la tarea se vuelve 20% mas facil */
#define MAX_NIVEL     4        /* freno TEMPORAL: nadie crea hijos mas alla de este nivel */

/* Mensaje privado que baja por el pipe: del padre al Mr. Meeseeks. */
typedef struct {
    char   solicitud[MAX_SOLICITUD];
    double dificultad;   /* 0 = imposible ... 100 = muy facil, o AL_AZAR */
    double espera;       /* segundos de simulacion, o AL_AZAR */
} Mensaje;

/* Respuesta que sube por el otro pipe: del Mr. Meeseeks a su padre. */
typedef struct {
    int    ok;           /* 1 = solicitud completada */
    double espera;       /* segundos dormidos en la simulacion (se restan del tiempo) */
    int    creados;      /* Mr. Meeseeks en este subarbol (yo + mis descendientes) */
    pid_t  pid, ppid;    /* datos del Mr. Meeseeks que resolvio la solicitud */
    int    N, i;
} Resultado;

/* ------------------------------------------------------------------
 * Variables compartidas entre TODOS los procesos (Box y Mr. Meeseeks).
 * Viven en memoria creada con mmap(MAP_SHARED) antes de cualquier fork,
 * asi que todos los hijos ven la misma memoria fisica (no una copia).
 * El semaforo "mutex" garantiza que solo un proceso a la vez las modifica.
 * ------------------------------------------------------------------ */
#define NIVELES_REG 64

typedef struct {
    sem_t mutex;                    /* semaforo binario administrado por el SO */
    int   creados;                  /* Meeseeks creados en la solicitud actual */
    int   vivos;                    /* Meeseeks existiendo en este momento     */
    int   max_vivos;                /* el mayor valor que alcanzo "vivos"      */
    int   nivel_max;                /* nivel mas profundo alcanzado            */
    int   por_nivel[NIVELES_REG];   /* cuantos se crearon en cada nivel        */
} Compartido;

static Compartido *shm;   /* puntero a la memoria compartida */

static void bloquear(void) {
#ifndef SIN_SEMAFORO
    while (sem_wait(&shm->mutex) < 0) ;   /* reintenta si una senal lo interrumpe */
#endif
}

static void desbloquear(void) {
#ifndef SIN_SEMAFORO
    sem_post(&shm->mutex);
#endif
}

/* Un Meeseeks nace: se anota en los contadores (seccion critica).
 * Se usa leer -> ceder CPU -> escribir para que, sin semaforo, la condicion
 * de carrera sea visible en los experimentos. */
static void registrar_nacimiento(int N) {
    bloquear();
    int c = shm->creados;
    int v = shm->vivos;
    sched_yield();
    shm->creados = c + 1;
    shm->vivos   = v + 1;
    if (shm->vivos > shm->max_vivos) shm->max_vivos = shm->vivos;
    if (N > shm->nivel_max)          shm->nivel_max = N;
    if (N < NIVELES_REG)             shm->por_nivel[N]++;
    desbloquear();
}

/* Un Meeseeks desaparece. */
static void registrar_muerte(void) {
    bloquear();
    int v = shm->vivos;
    sched_yield();
    shm->vivos = v - 1;
    desbloquear();
}

/* El Box deja los contadores en cero antes de cada solicitud. */
static void reiniciar_contadores(void) {
    bloquear();
    shm->creados = shm->vivos = shm->max_vivos = shm->nivel_max = 0;
    memset(shm->por_nivel, 0, sizeof shm->por_nivel);
    desbloquear();
}

/* Un registro por cada solicitud atendida; con esto se arma el informe final. */
typedef struct {
    char   texto[MAX_SOLICITUD];
    double duracion;     /* segundos reales */
    int    completada;   /* 1 = si, 0 = no */
    int    meeseeks;     /* cuantos Mr. Meeseeks se crearon para esta tarea */
    int    max_vivos;    /* cuantos existieron al mismo tiempo, como maximo */
    int    nivel_max;    /* nivel mas profundo alcanzado */
} Tarea;

static Tarea bitacora[MAX_TAREAS];
static int   total = 0;

/* Informe que da el Box al despedirse. */
static void informe(void) {
    int hechas = 0;

    printf("\n================ INFORME DEL BOX ================\n");
    printf("%-3s %-30s %12s %-10s %8s %6s %6s\n",
           "#", "Solicitud", "Duracion(s)", "Estado", "Meeseeks", "MaxSim", "Nivel");
    for (int k = 0; k < total; k++) {
        printf("%-3d %-30.30s %12.6f %-10s %8d %6d %6d\n", k + 1, bitacora[k].texto,
               bitacora[k].duracion,
               bitacora[k].completada ? "Terminada" : "Fallida",
               bitacora[k].meeseeks, bitacora[k].max_vivos, bitacora[k].nivel_max);
        hechas += bitacora[k].completada;
    }
    printf("-------------------------------------------------\n");
    printf("Tareas solicitadas: %d | Terminadas: %d | No terminadas: %d\n",
           total, hechas, total - hechas);
}

/* Reloj monotónico en segundos: no se ve afectado si cambia la hora del sistema. */
static double ahora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------
 * Parte A: utilidades de azar y simulacion.
 * ------------------------------------------------------------------ */

/* Numero al azar uniforme en [min, max]. */
static double azar(double min, double max) {
    return min + (max - min) * (rand() / (double)RAND_MAX);
}

/* Dificultad al azar con distribucion normal (media 50, desviacion 25),
 * generada con el metodo de Box-Muller y recortada al rango [0, 100].
 * La mayoria de tareas salen "intermedias"; las muy faciles o imposibles son raras. */
static double dificultad_al_azar(void) {
    double u1 = azar(1e-9, 1.0);
    double u2 = azar(0.0, 1.0);
    double z  = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);   /* normal(0,1) */
    double d  = 50.0 + 25.0 * z;
    if (d < 0)   d = 0;
    if (d > 100) d = 100;
    return d;
}

/* Duerme los segundos indicados (admite decimales). */
static void dormir(double segundos) {
    struct timespec t;
    t.tv_sec  = (time_t)segundos;
    t.tv_nsec = (long)((segundos - t.tv_sec) * 1e9);
    nanosleep(&t, NULL);
}

/* Simula el intento de un Mr. Meeseeks de cumplir una solicitud textual.
 * Duerme "espera" segundos y luego decide. Devuelve 1 si lo logro. */
static int intentar_tarea(double dificultad, double espera) {
    printf("[pid %d, ppid %d] Dificultad %.2f. Trabajando %.2f s...\n",
           getpid(), getppid(), dificultad, espera);
    dormir(espera);

    if (dificultad > 85.0) {           /* 85.01 a 100: un solo Meeseeks basta */
        printf("[pid %d, ppid %d] Era facil, puedo solo!\n", getpid(), getppid());
        return 1;
    }

    /* 0 a 85: lanza el dado. Gana si el dado cae por debajo de la dificultad. */
    double dado = azar(0.0, 100.0);
    int exito = dado < dificultad;
    printf("[pid %d, ppid %d] Dado: %.2f (necesitaba menos de %.2f) -> %s\n",
           getpid(), getppid(), dado, dificultad, exito ? "lo logre" : "no pude");
    return exito;
}

static void meeseeks(int fd_entrada, int fd_salida, int N, int i);

/* Parte A completa: el Mr. Meeseeks intenta la tarea y, si no puede, crea
 * otros Mr. Meeseeks (nivel N+1) y espera lo que le respondan por pipe.
 * fd_salida es el pipe hacia MI padre: mis hijos lo heredan y deben cerrarlo. */
static void resolver_textual(const Mensaje *m, int N, int i, int fd_salida, Resultado *r) {
    double dificultad = m->dificultad;
    if (dificultad == AL_AZAR) {
        dificultad = dificultad_al_azar();
        printf("[pid %d, ppid %d] Nadie me dijo la dificultad... la sorteo: %.2f\n",
               getpid(), getppid(), dificultad);
    }
    double espera = (m->espera == AL_AZAR) ? azar(0.5, 5.0) : m->espera;

    r->espera = espera;
    if (intentar_tarea(dificultad, espera)) {
        r->ok   = 1;                       /* yo mismo la resolvi */
        r->pid  = getpid();
        r->ppid = getppid();
        r->N    = N;
        r->i    = i;
        return;
    }

    if (N >= MAX_NIVEL) {
        printf("[pid %d, ppid %d] Ya somos demasiados niveles (%d), no pido mas ayuda.\n",
               getpid(), getppid(), N);
        return;
    }

    /* --- Pide ayuda: 5 hijos si es muy dificil (0-45), 2 si es intermedia (45.01-85) --- */
    int cantidad = (dificultad <= 45.0) ? 5 : 2;

    /* Anejamiento: la tarea se simplifica para el siguiente nivel.
     * Al ser una multiplicacion, una tarea imposible (0) sigue siendo 0. */
    Mensaje para_hijo = *m;
    para_hijo.dificultad = dificultad * ANEJAMIENTO;
    if (para_hijo.dificultad > 100.0) para_hijo.dificultad = 100.0;

    printf("[pid %d, ppid %d] No puedo solo! Creo %d Mr. Meeseeks (nivel %d, dificultad %.2f)\n",
           getpid(), getppid(), cantidad, N + 1, para_hijo.dificultad);

    pid_t pids[MAX_HIJOS];
    int   lee[MAX_HIJOS];    /* extremo de lectura del pipe "sube" de cada hijo */
    int   creados = 0;

    for (int k = 0; k < cantidad; k++) {
        int baja[2], sube[2];
        if (pipe(baja) < 0) { perror("pipe"); break; }
        if (pipe(sube) < 0) { perror("pipe"); close(baja[0]); close(baja[1]); break; }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(baja[0]); close(baja[1]);
            close(sube[0]); close(sube[1]);
            break;
        }
        if (pid == 0) {
            /* hijo: cierra todo lo heredado que no le pertenece */
            close(baja[1]);
            close(sube[0]);
            close(fd_salida);                              /* pipe de mi padre a mi abuelo */
            for (int j = 0; j < creados; j++) close(lee[j]);   /* pipes de mis hermanos */
            meeseeks(baja[0], sube[1], N + 1, k + 1);      /* nunca retorna */
        }

        /* padre: le pasa la solicitud por mensaje privado */
        close(baja[0]);
        close(sube[1]);
        if (write(baja[1], &para_hijo, sizeof para_hijo) < 0) perror("write");
        close(baja[1]);

        pids[creados] = pid;
        lee[creados]  = sube[0];
        creados++;
    }

    /* --- Espera la respuesta de cada hijo --- */
    double espera_hijos = 0.0;
    for (int k = 0; k < creados; k++) {
        Resultado h;
        if (read(lee[k], &h, sizeof h) == sizeof h) {
            r->creados += h.creados;
            if (h.espera > espera_hijos) espera_hijos = h.espera;
            if (h.ok && !r->ok) {          /* me quedo con el primero que lo logro */
                r->ok   = 1;
                r->pid  = h.pid;
                r->ppid = h.ppid;
                r->N    = h.N;
                r->i    = h.i;
                printf("[pid %d, ppid %d] Mi hijo %d me avisa: lo resolvio el Mr. Meeseeks (%d, %d, %d, %d)\n",
                       getpid(), getppid(), pids[k], h.pid, h.ppid, h.N, h.i);
            }
        }
        close(lee[k]);
        waitpid(pids[k], NULL, 0);
    }

    /* Los hijos trabajan en paralelo: el tiempo dormido que cuenta es el de la
     * cadena mas larga (mi espera + la mayor de mis hijos). */
    r->espera += espera_hijos;
}

/* ------------------------------------------------------------------
 * Parte B: calculadora (analizador descendente recursivo).
 * Cada funcion atiende un nivel de precedencia, de menor a mayor:
 *
 *   e_or    :  e_and  ( "||" e_and )*
 *   e_and   :  e_cmp  ( "&&" e_cmp )*
 *   e_cmp   :  e_suma ( ("==" | "!=" | "<" | "<=" | ">" | ">=") e_suma )*
 *   e_suma  :  e_mult ( ("+" | "-") e_mult )*
 *   e_mult  :  e_unario ( ("*" | "/" | "%") e_unario )*
 *   e_unario:  ("-" | "+" | "!") e_unario  |  e_primario
 *   e_primario: numero | "(" e_or ")"
 *
 * Los resultados logicos son 1 (verdadero) o 0 (falso), como en C.
 * ------------------------------------------------------------------ */
static const char *cur;        /* posicion actual en el texto */
static const char *calc_error; /* NULL si no ha habido error */

static double e_or(void);

static void saltar_espacios(void) {
    while (isspace((unsigned char)*cur)) cur++;
}

/* Si el texto continua con el operador op, lo consume y devuelve 1. */
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
    if (!calc_error) calc_error = msg;   /* se conserva el primer error */
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
    if (*cur == '!' && cur[1] != '=') {   /* "!" pero no "!=" */
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
    for (;;) {   /* los operadores de dos caracteres se revisan primero */
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

/* Evalua la expresion. Devuelve 1 y deja el valor en *resultado si todo salio
 * bien; devuelve 0 y deja el mensaje en *error si hubo un problema. */
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

/* Parte C: ejecuta un programa externo y espera su valor de retorno.
 * Devuelve 1 si el programa termino con exito (codigo 0), 0 si no. */
static int ejecutar_programa(const char *comando) {
    fflush(stdout);
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        /* Este proceso se convierte en el programa pedido.
         * Se usa el shell para que entienda argumentos, comillas, etc. */
        execl("/bin/sh", "sh", "-c", comando, (char *)NULL);
        perror("execl");   /* solo se llega aqui si exec fallo */
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

/* Código que ejecuta cada Mr. Meeseeks (proceso hijo). Nunca retorna.
 * fd_entrada: pipe por donde llega el Mensaje del padre.
 * fd_salida : pipe por donde se devuelve el Resultado al padre.
 * N: nivel, i: numero de instancia dentro de su nivel. */
static void meeseeks(int fd_entrada, int fd_salida, int N, int i) {
    Mensaje   m;
    Resultado r;

    memset(&r, 0, sizeof r);
    r.creados = 1;                 /* yo mismo */

    /* Cada proceso necesita su propia semilla; si no, todos los hijos
     * heredarian la misma secuencia de numeros "al azar" del padre. */
    srand((unsigned)(getpid() ^ time(NULL)));

    registrar_nacimiento(N);       /* contador global protegido con semaforo */

    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), N, i);

    /* read se bloquea hasta que el padre escriba en el pipe */
    ssize_t n = read(fd_entrada, &m, sizeof m);
    close(fd_entrada);
    if (n != sizeof m) {
        printf("[pid %d, ppid %d] No recibi ninguna solicitud\n", getpid(), getppid());
        exit(1);
    }

    printf("[pid %d, ppid %d] Recibi por pipe: \"%s\"\n",
           getpid(), getppid(), m.solicitud);

    /* Clasifica la solicitud segun su prefijo */
    if (strncmp(m.solicitud, "run ", 4) == 0) {
        r.ok = ejecutar_programa(m.solicitud + 4);          /* parte C */
    } else if (strncmp(m.solicitud, "calc ", 5) == 0) {
        double valor;
        const char *error;
        r.ok = calcular(m.solicitud + 5, &valor, &error);   /* parte B */
        if (r.ok)
            printf("[pid %d, ppid %d] Resultado: %g\n", getpid(), getppid(), valor);
        else
            printf("[pid %d, ppid %d] Error en el calculo: %s\n", getpid(), getppid(), error);
    } else {
        resolver_textual(&m, N, i, fd_salida, &r);          /* parte A */
    }

    if (r.ok && r.pid == 0) {      /* calc y run: quien resuelve soy yo */
        r.pid = getpid(); r.ppid = getppid(); r.N = N; r.i = i;
    }

    if (!r.ok)
        printf("[pid %d, ppid %d] Ooh, no pude completar la solicitud.\n",
               getpid(), getppid());
    else if (r.pid == getpid())
        printf("[pid %d, ppid %d] All done! Solicitud completada. Fue un placer servirle.\n",
               getpid(), getppid());
    else if (N > 1)
        printf("[pid %d, ppid %d] Le aviso a mi padre que ya esta resuelto.\n",
               getpid(), getppid());

    /* El Mr. Meeseeks original informa al usuario quien lo resolvio */
    if (N == 1 && r.ok)
        printf("[pid %d, ppid %d] Solicitud resuelta por el Mr. Meeseeks (%d, %d, %d, %d)\n",
               getpid(), getppid(), r.pid, r.ppid, r.N, r.i);

    printf("[pid %d, ppid %d] Adios! *poof*\n", getpid(), getppid());

    /* Comunica el resultado al padre por el pipe de vuelta */
    if (write(fd_salida, &r, sizeof r) < 0) perror("write");
    close(fd_salida);
    registrar_muerte();
    exit(r.ok ? 0 : 1);
}

/* Devuelve 1 si la solicitud es textual (parte A), 0 si es calc o run. */
static int es_textual(const char *solicitud) {
    return strncmp(solicitud, "run ", 4) != 0 && strncmp(solicitud, "calc ", 5) != 0;
}

/* Pregunta un numero al usuario. Con Enter (linea vacia) devuelve AL_AZAR.
 * El valor se recorta al rango [min, max]. */
static double preguntar(const char *texto, double min, double max) {
    char linea[64];

    printf("%s", texto);
    fflush(stdout);
    if (fgets(linea, sizeof linea, stdin) == NULL) return AL_AZAR;

    char *fin;
    double v = strtod(linea, &fin);
    if (fin == linea) return AL_AZAR;   /* no escribio ningun numero */
    if (v < min) v = min;
    if (v > max) v = max;
    return v;
}

int main(void) {
    char solicitud[MAX_SOLICITUD];

    /* Salida por lineas: cada mensaje se imprime de inmediato y ningun hijo
     * hereda texto pendiente en el buffer (evita mensajes duplicados). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Crea la memoria compartida ANTES de cualquier fork */
    shm = mmap(NULL, sizeof(Compartido), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* pshared = 1: el semaforo se comparte entre procesos; valor inicial 1 (libre) */
    if (sem_init(&shm->mutex, 1, 1) < 0) {
        perror("sem_init");
        return 1;
    }
#ifdef SIN_SEMAFORO
    printf("*** ATENCION: compilado SIN semaforo (experimento de condicion de carrera) ***\n");
#endif

    printf("=== Box Mr. Meeseeks === (escriba \"adios\" para salir)\n");

    while (1) {
        printf("Box> ");
        fflush(stdout);

        if (fgets(solicitud, sizeof solicitud, stdin) == NULL) break; /* EOF */
        solicitud[strcspn(solicitud, "\n")] = '\0';                   /* quita el \n */

        if (solicitud[0] == '\0') continue;
        if (strcmp(solicitud, "adios") == 0) break;

        if (total == MAX_TAREAS) {
            printf("[Box %d] Bitacora llena, no acepto mas solicitudes\n", getpid());
            break;
        }

        /* arma el mensaje privado para el Meeseeks */
        Mensaje m;
        strcpy(m.solicitud, solicitud);
        m.dificultad = AL_AZAR;
        m.espera     = AL_AZAR;
        if (es_textual(solicitud)) {
            m.dificultad = preguntar("  Dificultad (0-100, Enter = al azar): ", 0.0, 100.0);
            m.espera     = preguntar("  Tiempo en segundos (Enter = al azar 0.5-5): ", 0.0, 3600.0);
        }

        reiniciar_contadores();
        double t0 = ahora();

        int baja[2];   /* Box -> Meeseeks: el Mensaje   */
        int sube[2];   /* Meeseeks -> Box: el Resultado */
        if (pipe(baja) < 0) {
            perror("pipe");
            continue;
        }
        if (pipe(sube) < 0) {
            perror("pipe");
            close(baja[0]);
            close(baja[1]);
            continue;
        }

        fflush(stdout);          /* evita que el hijo herede texto pendiente en el buffer */
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            close(baja[0]); close(baja[1]);
            close(sube[0]); close(sube[1]);
            continue;
        }
        if (pid == 0) {
            close(baja[1]);                        /* el hijo lee de "baja"...   */
            close(sube[0]);                        /* ...y escribe en "sube"     */
            meeseeks(baja[0], sube[1], 1, 1);      /* nivel 1, instancia 1 */
        }

        /* padre (Box): escribe en "baja" y lee de "sube" */
        close(baja[0]);
        close(sube[1]);
        if (write(baja[1], &m, sizeof m) < 0) perror("write");
        close(baja[1]);

        /* espera la respuesta; read devuelve 0 si el hijo murio sin contestar */
        Resultado r;
        if (read(sube[0], &r, sizeof r) != sizeof r) {
            memset(&r, 0, sizeof r);
            r.creados = 1;
        }
        close(sube[0]);

        waitpid(pid, NULL, 0);   /* recoge al hijo para que no quede zombie */
        double t1 = ahora();

        /* anota la tarea en la bitacora */
        Tarea *t = &bitacora[total++];
        strcpy(t->texto, solicitud);
        t->duracion   = (t1 - t0) - r.espera;   /* tiempo real sin la espera simulada */
        t->completada = r.ok;
        t->meeseeks   = shm->creados;   /* todos ya terminaron: nadie mas lo modifica */
        t->max_vivos  = shm->max_vivos;
        t->nivel_max  = shm->nivel_max;
        int ok = r.ok;

        printf("[Box %d] Solicitud %d %s en %.6f s\n",
               getpid(), total, ok ? "completada" : "NO completada", t->duracion);
        printf("[Box %d] Meeseeks creados: %d (verificacion por pipes: %d) | "
               "maximo simultaneos: %d | vivos ahora: %d\n",
               getpid(), shm->creados, r.creados, shm->max_vivos, shm->vivos);
        printf("[Box %d] Por nivel:", getpid());
        for (int n = 1; n <= shm->nivel_max && n < NIVELES_REG; n++)
            printf(" N%d=%d", n, shm->por_nivel[n]);
        printf("\n");
    }

    informe();
    sem_destroy(&shm->mutex);
    munmap(shm, sizeof(Compartido));
    printf("[Box %d] Adios!\n", getpid());
    return 0;
}