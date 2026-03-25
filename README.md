# Chat App — CC3064 Sistemas Operativos
### Proyecto #1 | Universidad del Valle de Guatemala

Aplicación de chat cliente-servidor en C++17 con soporte para mensajes broadcast, mensajes directos, manejo de status y múltiples clientes concurrentes.

---

## Tecnologías

- **C++17** con pthreads
- **Protocol Buffers (protobuf)** — serialización de mensajes
- **ncurses** — interfaz de usuario en terminal
- **TCP Sockets** — comunicación en red
- **Multithreading** — `std::thread` por cliente en el servidor

---

## Requisitos

```bash
sudo apt update
sudo apt install -y g++ make libprotobuf-dev protobuf-compiler libncursesw5-dev
```

---

## Compilación

```bash
# 1. Generar archivos de protobuf
protoc --cpp_out=protos/ --proto_path=protos/ protos/*.proto

# 2. Compilar servidor y cliente
make all
```

---

## Uso

### Servidor
```bash
./server <puerto>

# Ejemplo:
./server 8080
```

### Cliente
```bash
./client <usuario> <IP_servidor> <puerto>

# Ejemplo local:
./client alice 127.0.0.1 8080

# Ejemplo en red LAN:
./client alice 192.168.1.10 8080
```

---

## Comandos del cliente

| Comando | Descripción |
|---|---|
| `<mensaje>` | Enviar mensaje a todos (broadcast) |
| `/dm <usuario> <mensaje>` | Mensaje directo privado |
| `/users` | Listar usuarios conectados y su status |
| `/info <usuario>` | Ver IP y status de un usuario |
| `/status <s>` | Cambiar status: `active`, `busy`, `inactive` |
| `/help` | Mostrar ayuda |
| `/quit` | Desconectarse y salir |

---

## Protocolo

Implementa el protocolo estándar acordado por la clase usando **Protocol Buffers** con framing TCP de 5 bytes:

```
┌─────────────┬──────────────────────────┬──────────────────┐
│ 1 byte type │ 4 bytes length (big-end) │ N bytes protobuf │
└─────────────┴──────────────────────────┴──────────────────┘
```

| Type | Dirección | Mensaje |
|------|-----------|---------|
| 1 | client → server | Register |
| 2 | client → server | MessageGeneral |
| 3 | client → server | MessageDM |
| 4 | client → server | ChangeStatus |
| 5 | client → server | ListUsers |
| 6 | client → server | GetUserInfo |
| 7 | client → server | Quit |
| 10 | server → client | ServerResponse |
| 11 | server → client | AllUsers |
| 12 | server → client | ForDm |
| 13 | server → client | BroadcastDelivery |
| 14 | server → client | GetUserInfoResponse |

---

## Funcionalidades del servidor

- **Multithreading** — un thread por cliente conectado
- **Registro de usuarios** — valida nombres duplicados e IPs duplicadas en red LAN
- **Liberación de usuarios** — detecta desconexión y notifica a todos
- **Broadcasting** — reenvía mensajes a todos los clientes conectados
- **Mensajes directos** — enruta mensajes entre usuarios específicos
- **Manejo de status** — ACTIVE, BUSY (DO_NOT_DISTURB), INACTIVE (INVISIBLE)
- **Inactividad automática** — cambia el status a INACTIVE tras 30 segundos sin actividad

---

## Estructura del proyecto

```
chat/
├── Makefile
├── README.md
├── include/
│   └── protocol.h        # Framing TCP (send/recv con header de 5 bytes)
├── protos/               # Definiciones .proto del protocolo de la clase
│   ├── common.proto
│   ├── register.proto
│   ├── message_general.proto
│   ├── message_dm.proto
│   ├── change_status.proto
│   ├── list_users.proto
│   ├── get_user_info.proto
│   ├── quit.proto
│   ├── all_users.proto
│   ├── broadcast_messages.proto
│   ├── for_dm.proto
│   ├── get_user_info_response.proto
│   └── server_response.proto
└── src/
    ├── server.cpp        # Servidor multithreaded
    └── client.cpp        # Cliente con interfaz ncurses
```

---

## Notas para la entrega

- El proyecto fue desarrollado y probado en **Ubuntu (WSL)** sobre Windows
- Para pruebas locales se permiten múltiples clientes desde `127.0.0.1`
- En red LAN cada IP solo puede tener un cliente conectado simultáneamente
- Para ver tu IP en la red del salón: `hostname -I`
