# fanctl
<!-- Modified by fanctl contributors on 2026-11-15: document control handoff and concise terminal output. -->

[English](README.md) · [简体中文](README.zh-Hans.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Español](README.es.md)

<p><img src="Assets/icon.svg" alt="fanctl" width="96" height="96"></p>

Un pequeño comando que uso en el terminal del Mac para leer el estado de los ventiladores y pedir una velocidad temporal. El producto es la CLI: cuatro comandos cortos, sin una ventana que mantener abierta.

## Cuatro comandos

```sh
fanctl stat       # read current state
fanctl 3000       # one RPM target for all fans
fanctl max        # each fan's own maximum
fanctl auto       # hand control back to macOS
```

`3000`, `max` y `auto` muestran las RPM medidas de cada ventilador antes → después. `stat` muestra solo las RPM actuales y el modo; `stat --verbose` añade objetivos, rangos y valores de modo. Los ventiladores se numeran desde 1. El objetivo numérico debe estar dentro del rango de todos; `max` usa el máximo de cada uno.

```text
Fan 01  ▰▰▰▰▰···········   2000 RPM  Manual
        from 2000 RPM  ·  target 2500
```

La terminal interactiva muestra las RPM medidas en una barra de ámbar a rojo, a escala del máximo de cada ventilador. En terminales estrechas se mantienen las cifras y el modo. La salida redirigida es texto sin animación. `NO_COLOR` / `--no-color` desactiva el color; `TERM=dumb` desactiva los adornos. Reducir movimiento de macOS para el usuario conectado desactiva la animación. La CLI sigue en inglés. Para automatización, `--non-interactive` prohíbe pedir contraseña y falla si no hay autorización.

La cesión de control se comprueba hasta 30 segundos, tratando todos los ventiladores juntos. Solo se reintenta la respuesta conocida de modo protegido (`0x82`); los demás errores de escritura siguen siendo fallos. El modo `3` es automático del sistema, no manual. Por defecto, se leen las RPM en cuanto se confirma el cambio. Con `--wait` se observan hasta ocho segundos; los valores finales son mediciones y un objetivo manual aún no alcanzado se indica aparte. Al recibir `SIGINT` / `SIGTERM` durante un cambio, intenta recuperar y verificar el modo automático antes de salir. La señal debe llegar a fanctl; detener solo el intermediario de autorización no es una vía de cancelación verificada.

## Compilar la fuente seleccionada

En la carpeta exportada con `fanctl.c`, usa Apple Command Line Tools. No se descargan paquetes externos.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## Permisos y estado

`stat` lee con los permisos del usuario y solo solicita autorización si IOKit deniega el acceso. Cambiar la velocidad requiere permisos de administrador (con `/usr/bin/sudo` en la compilación independiente). La del propietario comprueba primero la caché de sudo sin pedir contraseña. Si sigue válida, usa sudo no interactivo; de lo contrario, las solicitudes locales usan `sudo -A` con el helper nativo adyacente como askpass para crear la caché normal. SSH entrante conserva el flujo de terminal del helper. No guarda contraseñas ni cambia la política de sudo. `--help` y los argumentos inválidos no piden acceso. La opción de compilación `FANCTL_USE_ADJACENT_AUTHORIZER` es `0` por defecto y `1` para el instalador del propietario; otros valores impiden compilar. Si falta el helper elegido, falla de forma visible sin cambiar de vía. La reutilización sigue la política de sesiones de sudo; otra terminal puede necesitar su propia autorización.

Sale después de cada comando. El objetivo forzado sigue hasta que `auto`, macOS u otro controlador lo cambie. No instala daemon, inicio automático, perfiles guardados ni telemetría. La compatibilidad depende de las claves AppleSMC del Mac; no afirma aceptación en todos los modelos.

Si un cambio falla después de escribir en el controlador, fanctl intenta devolver todos los ventiladores al modo automático y verifica las claves de control. Cada transición de recuperación tiene un plazo de comprobación de 30 segundos. Si hay un desbloqueo aceptado pendiente, espera a que se aplique antes de liberarlo para evitar una activación tardía. No vuelve a escribir valores ya automáticos. Los fallos inesperados de escritura y las claves ilegibles siguen siendo errores. Informa de las claves que no puede confirmar y no restaura ajustes manuales anteriores. Los formatos desconocidos de claves de control y las respuestas SMC incompletas se notifican como errores. Una terminación forzada, un corte de energía o un controlador inaccesible pueden impedir la recuperación. Si es incompleta, ejecuta `fanctl auto` y comprueba `fanctl stat`.

En un Mac real se comprobaron un objetivo temporal y la vuelta al modo automático. La recuperación tras un fallo de escritura solo se ha simulado; no se ha verificado en otros Mac.

## Fuente y comprobaciones

La prueba simula la autorización y rechaza acceso SMC. Comprueba compilación/help, transferencia de argumentos y fallos. No cambia RPM ni demuestra hardware real. La salida CLI sigue en inglés. Código, documentos, pruebas y marca original seleccionados usan [GPL-2.0-only](LICENSE); conserva [los avisos de terceros](THIRD_PARTY_NOTICES.md).
