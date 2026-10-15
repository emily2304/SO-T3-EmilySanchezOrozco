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
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* Configuracion del programa */
#define MAX_SOLICITUD 256
#define MAX_TAREAS    100
#define MAX_HIJOS     5
#define MAX_FD        256
#define AL_AZAR       (-1.0)
#define ANEJAMIENTO   1.20
#define CANSANCIO     1.25
#define ESPERA_MAX    10.0
#define MINUTOS_DEF   5.0
#define MINUTOS_TOPE  1440.0
#define CUPO_DEF      200

/* Solicitud a cada Meeseeks */
typedef struct {
    char   solicitud[MAX_SOLICITUD];
    double dificultad;
    double espera;
} Mensaje;

/* Resultado que los Meeseeks devuelven */
typedef struct {
    int    ok;
    double espera;
    pid_t  pid, ppid;
    int    N, i;
} Resultado;

typedef struct {
    sem_t mutex;
    int   creados;
    int   vivos;
    int   max_vivos;
    int   nivel_max;
    int   cupo;
    int   sin_cupo;
} Compartido;

static Compartido *shm;

static void bloquear(void) {
#ifndef SIN_SEMAFORO
    while (sem_wait(&shm->mutex) < 0) ;
#endif
}

static void desbloquear(void) {
#ifndef SIN_SEMAFORO
    sem_post(&shm->mutex);
#endif
}

/* Registra la creacion de un Meeseeks */
static void registrar_nacimiento(int N) {
    bloquear();
    int c = shm->creados;
    sched_yield();
    shm->creados = c + 1;
    if (N > shm->nivel_max) shm->nivel_max = N;
    desbloquear();
}

/* Actualiza la cantidad de procesos activos */
static int ajustar_vivos(int cantidad) {
    bloquear();
    int v = shm->vivos;
    sched_yield();
    int ok = (v + cantidad <= shm->cupo) || cantidad < 0;
    if (ok) {
        shm->vivos = v + cantidad;
        if (shm->vivos > shm->max_vivos) shm->max_vivos = shm->vivos;
    } else {
        shm->sin_cupo++;
    }
    desbloquear();
    return ok;
}

static void decir(const char *formato, ...) {
    va_list args;
    va_start(args, formato);
    printf("[Proceso %d | Padre %d] ", getpid(), getppid());
    vprintf(formato, args);
    printf("\n");
    va_end(args);
}

/* Tiempo monotono en segundos */
static double ahora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Genera un numero aleatorio entre min y max */
static double azar(double min, double max) {
    return min + (max - min) * (rand() / (double)RAND_MAX);
}

/* Selecciona una dificultad aleatoria, normalmente intermedia */
static double dificultad_al_azar(void) {
    double z = sqrt(-2.0 * log(azar(1e-9, 1.0))) * cos(2.0 * M_PI * azar(0.0, 1.0));
    return fmin(fmax(50.0 + 25.0 * z, 0.0), 100.0);
}

static void meeseeks(int entrada, int salida, int N, int i);

/* Crea un proceso ayudante y le envia la solicitud */
static pid_t crear_meeseeks(const Mensaje *m, int N, int i, int *lee, int *escribe) {
    int baja[2], sube[2];
    if (pipe(baja) < 0) return -1;
    if (pipe(sube) < 0) { close(baja[0]); close(baja[1]); return -1; }

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (N == 1) setpgid(0, 0);
        for (int fd = 3; fd < MAX_FD; fd++)
            if (fd != baja[0] && fd != sube[1]) close(fd);
        meeseeks(baja[0], sube[1], N, i);
    }

    close(baja[0]);
    close(sube[1]);
    if (pid < 0) {
        close(baja[1]);
        close(sube[0]);
        return -1;
    }
    if (N == 1) setpgid(pid, pid);
    if (write(baja[1], m, sizeof *m) < 0) perror("No se pudo enviar la solicitud");
    *lee     = sube[0];
    *escribe = baja[1];
    return pid;
}

/* Simula un intento de resolver la solicitud */
static int intentar_tarea(double dificultad, double espera, int entrada, double *dormido) {
    decir("Intentando resolver la tarea (dificultad: %.1f/100). Tiempo de trabajo: %.2f s.",
          dificultad, espera);

    double t0 = ahora();
    struct pollfd p = { .fd = entrada, .events = POLLIN };
    int orden = poll(&p, 1, (int)(espera * 1000)) > 0;
    *dormido += ahora() - t0;
    if (orden) return -1;

    if (dificultad > 85.0) {
        decir("La tarea parece sencilla. Puedo resolverla sin ayuda.");
        return 1;
    }
    double dado = azar(0.0, 100.0);
    int exito = dado < dificultad;
    decir("Resultado del intento: %s (valor obtenido: %.2f; se necesitaba menos de %.2f).",
          exito ? "exitoso" : "no exitoso", dado, dificultad);
    return exito;
}

/* Avisa a los procesos ayudantes que pueden finalizar */
static void despedir_hijos(pid_t pid[], int escribe[], int n, int menos) {
    for (int k = 0; k < n; k++) {
        if (k == menos || escribe[k] < 0) continue;
        decir("El ayudante %d ya no es necesario. Se le solicita finalizar.", pid[k]);
        close(escribe[k]);
        escribe[k] = -1;
    }
}

/* Crea ayudantes y espera a que alguno resuelva la solicitud */
static int pedir_ayuda(const Mensaje *m, double dificultad, int cantidad, int N,
                       int entrada, Resultado *r) {
    Mensaje para_hijo = *m;
    para_hijo.dificultad = fmin(dificultad * ANEJAMIENTO, 100.0);
    decir("No pude resolver la tarea por mi cuenta. Creando %d ayudante(s) (nivel %d, dificultad asignada: %.1f/100).",
          cantidad, N + 1, para_hijo.dificultad);

    pid_t pid[MAX_HIJOS];
    int   lee[MAX_HIJOS], escribe[MAX_HIJOS];
    int   n = 0;
    while (n < cantidad) {
        pid[n] = crear_meeseeks(&para_hijo, N + 1, n + 1, &lee[n], &escribe[n]);
        if (pid[n] < 0) { perror("No se pudo crear un proceso ayudante"); break; }
        n++;
    }
    if (n < cantidad) ajustar_vivos(n - cantidad);

    double espera_hijos = 0.0;
    int    cancelado = 0, restantes = n;

    while (restantes > 0) {
        struct pollfd p[MAX_HIJOS + 1];
        p[0] = (struct pollfd){ .fd = (cancelado || r->ok) ? -1 : entrada, .events = POLLIN };
        for (int k = 0; k < n; k++)
            p[k + 1] = (struct pollfd){ .fd = lee[k], .events = POLLIN };
        if (poll(p, n + 1, -1) < 0) continue;

        if (p[0].revents) {
            decir("Recibi una orden de finalizar. Avisare a mis ayudantes.");
            despedir_hijos(pid, escribe, n, -1);
            cancelado = 1;
        }

        for (int k = 0; k < n; k++) {
            if (lee[k] < 0 || !p[k + 1].revents) continue;

            Resultado h;
            if (read(lee[k], &h, sizeof h) == sizeof h) {
                espera_hijos = fmax(espera_hijos, h.espera);
                if (h.ok && !r->ok) {
                    r->ok = 1;
                    r->pid = h.pid; r->ppid = h.ppid; r->N = h.N; r->i = h.i;
                    decir("El ayudante %d informa que la solicitud fue resuelta por el proceso %d (padre %d, nivel %d, ayudante %d).",
                          pid[k], h.pid, h.ppid, h.N, h.i);
                    despedir_hijos(pid, escribe, n, k);
                }
            }
            close(lee[k]);
            lee[k] = -1;
            if (escribe[k] >= 0) { close(escribe[k]); escribe[k] = -1; }
            waitpid(pid[k], NULL, 0);
            restantes--;
        }
    }

    r->espera += espera_hijos;
    return cancelado;
}

/* Atiende una solicitud de texto, intentando resolverla o pidiendo ayuda */
static int resolver_textual(const Mensaje *m, int N, int entrada, Resultado *r) {
    double dificultad = m->dificultad;
    double espera     = (m->espera == AL_AZAR) ? azar(0.5, 5.0) : m->espera;
    if (dificultad == AL_AZAR) {
        dificultad = dificultad_al_azar();
        decir("No se indico la dificultad. Se asigno automaticamente: %.1f/100.", dificultad);
    }

    for (;;) {
        int intento = intentar_tarea(dificultad, espera, entrada, &r->espera);
        if (intento != 0) {
            r->ok = (intento == 1);
            return intento == -1;
        }

        int ayudantes = (dificultad <= 45.0) ? 5 : 2;
        if (ajustar_vivos(ayudantes))
            return pedir_ayuda(m, dificultad, ayudantes, N, entrada, r);

        dificultad = fmin(dificultad * ANEJAMIENTO, 100.0);
        espera     = fmin(espera * CANSANCIO, ESPERA_MAX);
        decir("No hay espacio disponible para %d ayudante(s) (limite actual: %d procesos activos). Volvere a intentarlo con mayor dificultad y tiempo de trabajo.",
              ayudantes, shm->cupo);
    }
}

/* Calculadora: evalua operaciones respetando la precedencia */
static const char *cur;
static const char *calc_error;

static double e_or(void);

static int viene(const char *op) {
    while (isspace((unsigned char)*cur)) cur++;
    size_t n = strlen(op);
    if (strncmp(cur, op, n) != 0) return 0;
    cur += n;
    return 1;
}

static void fallo(const char *msg) {
    if (!calc_error) calc_error = msg;
}

static double e_primario(void) {
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
    fallo(*cur ? "se encontro un simbolo inesperado" : "la expresion esta incompleta");
    return 0;
}

static double e_unario(void) {
    if (viene("-")) return -e_unario();
    if (viene("+")) return +e_unario();
    if (cur[0] == '!' && cur[1] != '=') {
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
            if (d == 0) { fallo("no se puede dividir entre cero"); return 0; }
            v /= d;
        } else if (viene("%")) {
            double d = e_unario();
            if (d == 0) { fallo("no se puede calcular el modulo con divisor cero"); return 0; }
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

/* Muestra el resultado del calculo o explica el error */
static int calcular(const char *expresion) {
    cur = expresion;
    calc_error = NULL;
    double v = e_or();
    viene("");
    if (*cur != '\0') fallo("se encontro un simbolo inesperado");

    if (calc_error) {
        decir("No se pudo realizar el calculo: %s.", calc_error);
        return 0;
    }
    decir("Resultado del calculo: %g.", v);
    return 1;
}

/* Ejecuta un comando externo y muestra como termino */
static int ejecutar_programa(const char *comando) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("No se pudo iniciar el comando");
        return 0;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", comando, (char *)NULL);
        perror("No se pudo ejecutar el comando");
        _exit(127);
    }

    int estado;
    waitpid(pid, &estado, 0);
    if (WIFEXITED(estado)) {
        int codigo = WEXITSTATUS(estado);
        decir("El comando finalizo con %s (codigo de salida: %d).",
              codigo == 0 ? "exito" : "errores", codigo);
        return codigo == 0;
    }
    if (WIFSIGNALED(estado))
        decir("El comando termino inesperadamente por una senal del sistema (%d).", WTERMSIG(estado));
    return 0;
}

/* Vida de un Mr. Meeseeks: recibe la solicitud, la procesa y responde */
static void meeseeks(int entrada, int salida, int N, int i) {
    Mensaje   m;
    Resultado r = {0};
    int       cancelado = 0;

    srand((unsigned)(getpid() ^ time(NULL)));
    registrar_nacimiento(N);
    decir("Hola, soy Mr. Meeseeks. Estoy listo para ayudar (proceso %d, padre %d, nivel %d, ayudante %d).",
          getpid(), getppid(), N, i);

    if (read(entrada, &m, sizeof m) != sizeof m) {
        decir("No pude recibir la solicitud. Finalizare.");
        ajustar_vivos(-1);
        exit(1);
    }
    decir("Solicitud recibida: \"%s\".", m.solicitud);

    if (strncmp(m.solicitud, "run ", 4) == 0)
        r.ok = ejecutar_programa(m.solicitud + 4);
    else if (strncmp(m.solicitud, "calc ", 5) == 0)
        r.ok = calcular(m.solicitud + 5);
    else
        cancelado = resolver_textual(&m, N, entrada, &r);

    if (r.ok && r.pid == 0) {
        r.pid = getpid(); r.ppid = getppid(); r.N = N; r.i = i;
    }

    if (!r.ok && cancelado)
        decir("La tarea fue cancelada porque otro proceso ya no necesitaba ayuda.");
    else if (!r.ok)
        decir("No fue posible completar la solicitud.");
    else if (r.pid == getpid())
        decir("Solicitud completada correctamente. ¡Trabajo terminado!");
    else if (N > 1)
        decir("La tarea ya fue resuelta. Informare al proceso que me solicito ayuda.");

    if (N == 1 && r.ok)
        decir("Solicitud resuelta por el proceso %d (padre %d, nivel %d, ayudante %d).",
              r.pid, r.ppid, r.N, r.i);

    if (write(salida, &r, sizeof r) < 0) perror("No se pudo enviar el resultado");
    decir("Proceso finalizado.");
    ajustar_vivos(-1);
    exit(r.ok ? 0 : 1);
}

/* Datos para el resumen final */
typedef struct {
    char   texto[MAX_SOLICITUD];
    double duracion;
    double exceso;
    int    completada;
    int    caos;
    int    meeseeks, max_vivos, nivel_max;
} Tarea;

static Tarea bitacora[MAX_TAREAS];
static int   total = 0;

/* Guarda la informacion de una solicitud para el resumen */
static void anotar(const char *texto, double duracion, double exceso, int completada, int caos) {
    Tarea *t = &bitacora[total++];
    snprintf(t->texto, sizeof t->texto, "%s", texto);
    t->duracion   = duracion;
    t->exceso     = exceso;
    t->completada = completada;
    t->caos       = caos;
    t->meeseeks   = shm->creados;
    t->max_vivos  = shm->max_vivos;
    t->nivel_max  = shm->nivel_max;
}

/* Muestra un resumen de todas las solicitudes procesadas */
static void informe(void) {
    int hechas = 0;
    printf("\n================ RESUMEN DE SOLICITUDES ================\n");
    printf("%-3s %-30s %12s %10s %-14s %10s %12s %12s\n", "#", "Solicitud",
           "Duracion (s)", "Tiempo extra", "Estado", "Procesos", "Max. activos", "Nivel max.");
    for (int k = 0; k < total; k++) {
        Tarea *t = &bitacora[k];
        printf("%-3d %-30.30s %12.2f %10.2f %-14s %10d %12d %12d\n", k + 1, t->texto,
               t->duracion, t->exceso,
               t->caos ? "Tiempo agotado" : t->completada ? "Completada" : "No completada",
               t->meeseeks, t->max_vivos, t->nivel_max);
        hechas += t->completada;
    }
    printf("--------------------------------------------------------------------------\n");
    printf("Solicitudes recibidas: %d | Completadas: %d | No completadas: %d\n",
           total, hechas, total - hechas);
}

/* Solicita un numero; Enter permite seleccionar un valor automatico */
static double preguntar(const char *texto, double min, double max) {
    char linea[64], *fin;
    printf("%s", texto);
    fflush(stdout);
    if (fgets(linea, sizeof linea, stdin) == NULL) return AL_AZAR;
    double v = strtod(linea, &fin);
    if (fin == linea) return AL_AZAR;
    return fmin(fmax(v, min), max);
}

/* Si se agota el tiempo, finaliza los procesos de la solicitud */
static void caos_planetario(pid_t raiz, int lee, int escribe) {
    printf("\n[Box %d] Se alcanzo el tiempo limite. Se solicitara la finalizacion de los procesos.\n", getpid());
    printf("[Box %d] Procesos activos en este momento: %d.\n", getpid(), shm->vivos);
    close(escribe);

    struct pollfd p = { .fd = lee, .events = POLLIN };
    if (poll(&p, 1, 3000) == 0)
        printf("[Box %d] Los procesos no finalizaron a tiempo. Se cerraran por la fuerza.\n", getpid());
    kill(-raiz, SIGKILL);
    waitpid(raiz, NULL, 0);
    close(lee);
}

/* Programa principal: recibe solicitudes hasta que el usuario escriba adios */
int main(int argc, char *argv[]) {
    char solicitud[MAX_SOLICITUD];

    double minutos = (argc > 1) ? atof(argv[1])
                   : preguntar("Tiempo limite por solicitud, en minutos (Enter = 5): ",
                               0.0, MINUTOS_TOPE);
    int    cupo    = (argc > 2) ? atoi(argv[2]) : CUPO_DEF;
    double extra   = (argc > 3) ? atof(argv[3]) : 0.0;
    if (minutos <= 0) minutos = MINUTOS_DEF;
    if (cupo < 1)     cupo    = CUPO_DEF;
    minutos = fmin(minutos, MINUTOS_TOPE);
    extra   = fmin(fmax(extra, 0.0), MINUTOS_TOPE);

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) { perror("No se pudo reservar memoria compartida"); return 1; }
    if (sem_init(&shm->mutex, 1, 1) < 0) { perror("No se pudo inicializar el semaforo"); return 1; }
    shm->cupo = cupo;

#ifdef SIN_SEMAFORO
    printf("ADVERTENCIA: el programa se compilo sin semaforos; los contadores compartidos podrian ser incorrectos.\n");
#endif
    printf("\n=== BOX MR. MEESEEKS ===\n");
    printf("Escriba una solicitud y presione Enter. Escriba \"adios\" para salir.\n");
    printf("Tiempo limite por solicitud: %.2f min | Maximo de procesos activos: %d\n", minutos, cupo);
    if (extra > 0)
        printf("Modo de prueba: si se alcanza el limite, se esperaran %.2f min adicionales.\n", extra);

    while (total < MAX_TAREAS) {
        printf("\nSolicitud> ");
        fflush(stdout);
        if (fgets(solicitud, sizeof solicitud, stdin) == NULL) break;
        solicitud[strcspn(solicitud, "\n")] = '\0';
        if (solicitud[0] == '\0') continue;
        if (strcmp(solicitud, "adios") == 0) break;

        Mensaje m = { .dificultad = AL_AZAR, .espera = AL_AZAR };
        snprintf(m.solicitud, sizeof m.solicitud, "%s", solicitud);
        if (strncmp(solicitud, "run ", 4) != 0 && strncmp(solicitud, "calc ", 5) != 0) {
            m.dificultad = preguntar("  Dificultad de la tarea (0-100; Enter = automatica): ", 0.0, 100.0);
            m.espera     = preguntar("  Tiempo de trabajo por intento en segundos (Enter = automatico): ", 0.0, 3600.0);
        }

        bloquear();
        shm->creados = shm->vivos = shm->max_vivos = shm->nivel_max = shm->sin_cupo = 0;
        desbloquear();
        ajustar_vivos(1);

        double t0 = ahora();
        int lee, escribe;
        pid_t pid = crear_meeseeks(&m, 1, 1, &lee, &escribe);
        if (pid < 0) {
            perror("No se pudo iniciar el proceso principal de la solicitud");
            continue;
        }

        struct pollfd p = { .fd = lee, .events = POLLIN };
        int listo = poll(&p, 1, (int)(minutos * 60000)) > 0;
        if (!listo && extra > 0) {
            printf("[Box %d] Se alcanzo el limite de %.2f min y la solicitud sigue en proceso. Se esperaran %.2f min adicionales.\n",
                   getpid(), minutos, extra);
            listo = poll(&p, 1, (int)(extra * 60000)) > 0;
        }
        if (!listo) {
            caos_planetario(pid, lee, escribe);
            double real = ahora() - t0;
            anotar(solicitud, real, real - minutos * 60.0, 0, 1);
            printf("[Box %d] La ejecucion se detuvo porque se agoto el tiempo limite.\n", getpid());
            break;
        }

        Resultado r = {0};
        if (read(lee, &r, sizeof r) != sizeof r) memset(&r, 0, sizeof r);
        close(lee);
        close(escribe);
        waitpid(pid, NULL, 0);

        double real   = ahora() - t0;
        double exceso = fmax(real - minutos * 60.0, 0.0);
        anotar(solicitud, real - r.espera, exceso, r.ok, 0);
        if (exceso > 0)
            printf("[Box %d] La solicitud supero el limite por %.2f s.\n",
                   getpid(), exceso);
        printf("[Box %d] Solicitud %d: %s. Tiempo registrado: %.2f s (se excluyen %.2f s de espera simulada).\n",
               getpid(), total, r.ok ? "completada" : "no completada",
               bitacora[total - 1].duracion, r.espera);
        printf("[Box %d] Procesos creados: %d | Maximo simultaneo: %d | Nivel maximo: %d\n",
               getpid(), shm->creados, shm->max_vivos, shm->nivel_max);
        if (shm->sin_cupo > 0)
            printf("[Box %d] Aviso: se intento solicitar ayuda %d vez/veces, pero no habia espacio disponible.\n",
                   getpid(), shm->sin_cupo);
    }

    informe();
    sem_destroy(&shm->mutex);
    munmap(shm, sizeof *shm);
    printf("\n[Box %d] Programa finalizado. ¡Hasta luego!\n", getpid());
    return 0;
}
