# Box Mr. Meeseeks

Instituto Tecnológico de Costa Rica.
Tarea 3 
IC-6600 - Principios de Sistemas Operativos
Emily Sánchez Orozco - 2021067314

Programa en C para Linux inspirado en la caja de Mr. Meeseeks de *Rick and Morty*.
El usuario escribe una solicitud y el Box crea un proceso (un Mr. Meeseeks) que existe solo para cumplirla. Cuando termina, se despide y desaparece.

## Tipos de solicitud

| Tipo | Ejemplo | Qué pasa |
| --- | --- | --- |
| Deseo | `Quiero ser feliz` | Se simula con una dificultad (0 = imposible, 100 = muy fácil). Si es difícil, el Meeseeks crea ayudantes |
| Cálculo | `calc (3 + 4) * 2` | Resuelve operaciones aritméticas y lógicas |
| Programa | `run ls -l` | Ejecuta el programa y revisa si terminó bien (código 0) |

Para salir se escribe `adios`. El Box muestra un informe con la duración de cada solicitud, si se cumplió y cuántos Meeseeks necesitó.

## Requisitos

- Linux (Se puede usar WSL)
- `gcc` y `make`: `sudo apt install build-essential`

## Compilar

```bash
make
```

## Uso

```bash
./box [minutos_maximos] [maximo_de_meeseeks] [minutos_extra]
```

| Argumento | Qué es | Por defecto |
| --- | --- | --- |
| `minutos_maximos` | Tiempo máximo por solicitud antes del caos planetario | Se pregunta al iniciar (Enter = 5) |
| `maximo_de_meeseeks` | Cuántos Meeseeks pueden existir al mismo tiempo | 200 |
| `minutos_extra` | Solo para experimentos: tiempo adicional que se espera después del máximo | 0 |

Ejemplo de sesión:

```text
$ ./box 5
Box> Quiero ser feliz
  Dificultad (0-100, Enter = al azar): 50
  Tiempo en segundos (Enter = al azar 0.5-5):
Hi, I'm Mr. Meeseeks! Look at Meeeee. (12228, 12227, 1, 1)
...
Box> calc 10 / 4
Box> run ls -l
Box> adios
```

## Cómo funciona

- Cada Mr. Meeseeks es un proceso creado con `fork`; todos ejecutan el mismo código.
- Cada uno se comunica con su padre por dos **pipes**: uno para recibir la solicitud y otro para responder.
- Los contadores globales (Meeseeks creados, vivos, nivel máximo) están en **memoria compartida**, protegidos por un **semáforo**.
- Un **límite de Meeseeks vivos** evita que una tarea imposible colapse el sistema.
- Si una solicitud pasa del **tiempo máximo**, se declara el caos planetario y se terminan todos los procesos.
