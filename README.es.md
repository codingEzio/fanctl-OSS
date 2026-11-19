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

Ejecuta `fanctl` o `fanctl stat` para ver las RPM actuales de cada ventilador. La velocidad debe estar dentro del rango de todos los ventiladores. `max` usa el máximo de cada uno.

```text
Fan 1  2000 RPM
Fan 2  2016 RPM
```

Con el acceso sin contraseña activado, fanctl muestra las RPM actuales y devuelve el control de inmediato. Un proceso temporal aplica y verifica el último ajuste solicitado. Un fallo se muestra en la siguiente consulta con `fanctl`. La transición puede tardar varios segundos si los ventiladores estaban parados. Sin esta configuración, el comando espera la confirmación.

## Compilar la fuente seleccionada

En la carpeta exportada con `fanctl.c`, usa Apple Command Line Tools. No se descargan paquetes externos.

```sh
xcrun clang -std=c17 -O3 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation fanctl.c -o fanctl
./fanctl --help
sh tests/fanctl_test.sh
```

## Permisos y estado

La consulta y la repetición de un objetivo manual ya aplicado solo necesitan lectura. Los cambios usan sudo y reutilizan su autorización. La compilación gestionada admite una ventana de contraseña local. SSH usa su terminal. Sin terminal ni autorización, el comando termina de inmediato. Se puede autorizar el ejecutable de root en `/Library/PrivilegedHelperTools/fanctl` solo para valores numéricos, `auto` y `max`.

Sale después de cada comando. El objetivo forzado sigue hasta que `auto`, macOS u otro controlador lo cambie. No instala daemon, inicio automático, perfiles guardados ni telemetría. La compatibilidad depende de las claves AppleSMC del Mac; no afirma aceptación en todos los modelos.

Si un cambio falla después de escribir en el controlador, fanctl intenta devolver todos los ventiladores al modo automático y verifica las claves de control. Cada transición de recuperación tiene un plazo de comprobación de 30 segundos. Si hay un desbloqueo aceptado pendiente, espera a que se aplique antes de liberarlo para evitar una activación tardía. No vuelve a escribir valores ya automáticos. Los fallos inesperados de escritura y las claves ilegibles siguen siendo errores. Informa de las claves que no puede confirmar y no restaura ajustes manuales anteriores. Los formatos desconocidos de claves de control y las respuestas SMC incompletas se notifican como errores. Una terminación forzada, un corte de energía o un controlador inaccesible pueden impedir la recuperación. Si es incompleta, ejecuta `fanctl auto` y comprueba `fanctl stat`.

En un Mac real se comprobaron un objetivo temporal y la vuelta al modo automático. La recuperación tras un fallo de escritura solo se ha simulado; no se ha verificado en otros Mac.

## Fuente y comprobaciones

La prueba simula la autorización y rechaza acceso SMC. Comprueba compilación/help, transferencia de argumentos y fallos. No cambia RPM ni demuestra hardware real. La salida CLI sigue en inglés. Código, documentos, pruebas y marca original seleccionados usan [GPL-2.0-only](LICENSE); conserva [los avisos de terceros](THIRD_PARTY_NOTICES.md).
