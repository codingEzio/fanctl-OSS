# fanctl
<!-- Modified by fanctl contributors on 2026-11-07: document automatic recovery. -->

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

`stat` muestra velocidad actual, objetivo, rango permitido y modo automático/forzado. Un objetivo numérico debe estar dentro del rango de cada ventilador. La velocidad actual puede tardar en alcanzar el objetivo. `max` usa el máximo de cada ventilador.

## Compilar la fuente seleccionada

En la carpeta exportada con `fanctl.c`, usa Apple Command Line Tools. No se descargan paquetes externos.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## Permisos y estado

La compilación independiente usa `/usr/bin/sudo` para comandos SMC, incluido `stat`. La del propietario selecciona su helper adyacente existente. `--help` y los argumentos inválidos no piden acceso. La opción de compilación `FANCTL_USE_ADJACENT_AUTHORIZER` es `0` por defecto y `1` para el instalador del propietario; otros valores impiden compilar. Si falta el helper elegido, falla de forma visible sin cambiar de vía.

Sale después de cada comando. El objetivo forzado sigue hasta que `auto`, macOS u otro controlador lo cambie. No instala daemon, inicio automático, perfiles guardados ni telemetría. La compatibilidad depende de las claves AppleSMC del Mac; no afirma aceptación en todos los modelos.

Si un cambio falla después de escribir en el controlador, fanctl intenta devolver todos los ventiladores al modo automático y verifica las claves de control. Puede volver a comprobar hasta ocho veces las claves válidas que no sean cero; los fallos de lectura o escritura siguen siendo errores. Informa de las claves que no puede confirmar y no restaura ajustes manuales anteriores. Los formatos desconocidos de claves de control y las respuestas SMC incompletas se notifican como errores. Una terminación forzada, un corte de energía o un controlador inaccesible pueden impedir la recuperación. Si es incompleta, ejecuta `fanctl auto` y comprueba `fanctl stat`.

En un Mac real se comprobaron un objetivo temporal y la vuelta al modo automático. La recuperación tras un fallo de escritura solo se ha simulado; no se ha verificado en otros Mac.

## Fuente y comprobaciones

La prueba simula la autorización y rechaza acceso SMC. Comprueba compilación/help, transferencia de argumentos y fallos. No cambia RPM ni demuestra hardware real. La salida CLI sigue en inglés. Código, documentos, pruebas y marca original seleccionados usan [GPL-2.0-only](LICENSE); conserva [los avisos de terceros](THIRD_PARTY_NOTICES.md).
