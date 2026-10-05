# Pantallas — Override de `MASTER.md`

> Especificación de las tres vistas de la app (Historial · Dashboard ·
> Ajustes) y de la barra de navegación. Prevalece sobre `MASTER.md` en lo que
> contradiga. Tokens, tipografía, formas y anti-patrones de Master siguen
> vigentes.

## Navegación inferior

Barra fija abajo, ancho de la columna (máx. 520 px), con tres destinos en este
orden: **Historial · Dashboard · Ajustes** (el dashboard al centro, vista por
defecto).

- Rutas por hash: `#/history`, `#/dashboard`, `#/settings`.
- Cada destino es un `<a>` de ≥ 56 px de alto con ícono (22 px) sobre etiqueta
  (12 px). El activo lleva `aria-current="page"`, ícono en `--color-primary`
  sobre una cápsula `--color-primary-tint` de 60 × 32 px, y etiqueta en negrita
  color `--color-foreground`; los inactivos van en `--color-muted`.
- Fondo `--color-surface`, filete superior `--color-line`, padding inferior con
  `env(safe-area-inset-bottom)`. El contenido reserva ese alto para que nada
  quede tapado.
- Al cambiar de vista: scroll arriba, `document.title` = "PulsOx — <vista>" y el
  foco pasa al `h1` de la vista.

## Dashboard

Entra en una sola pantalla de 390 × 844 (sin scroll), de arriba abajo:

1. **Cabecera:** marca "PulsOx" a la izquierda; a la derecha el pill de conexión
   (`En vivo` / `Conectado` / `Demo` / `Reconectando…` / `Sin conexión`, siempre
   con punto + texto) y el chip de batería (solo si el dispositivo la informa;
   `< 20 %` → ícono ámbar y "· baja").
2. **Titular de estado** (h1, 26 px) con ícono de estado, y una línea de apoyo.
   Es lenguaje natural, no una etiqueta:

   | Situación | Titular |
   |---|---|
   | Sin conexión / conectando | "Sin conexión" / "Conectando…" |
   | Conectado, sin medición | "Listo para medir" — "Presioná el botón de medición del dispositivo para empezar." |
   | Midiendo, sin dedo | "Esperando lectura" |
   | Midiendo, señal sin estabilizar | "Calibrando…" |
   | Midiendo, todo normal | "Todo en rango" |
   | Midiendo, algún valor en precaución | "Fuera del rango habitual" |
   | Midiendo, algún valor en peligro | "Valores críticos" + motivo + "Si te sentís mal, buscá atención médica." |

   Durante la medición la línea de apoyo lleva el cronómetro ("Midiendo · 00:23").
3. **Tarjeta SpO₂:** gauge de anillo abierto (270°, escala fija 80–100 %) con el
   valor en 66 px al centro y el pill de estado en el hueco inferior. El arco
   toma el color del estado, con halo suave; la pista queda en `--color-track`.
   Dos muescas marcan los umbrales de "Precaución" y "Normal" (siguen a los
   umbrales configurados).
4. **Tarjeta frecuencia cardíaca:** valor en 52 px + "lpm" y, a la derecha, el
   pill de estado.
5. **Tarjeta Señal PPG:** título + etiqueta "Filtrada" y, a la derecha, la
   calidad de señal en cinco barras (solo con dedo). Debajo, la traza en vivo:

   - Ventana de 6 s, dibujada en canvas; una línea punteada por segundo, línea
     base y rótulos "−6 s" / "ahora".
   - Trazo violeta (`--color-violet`) que se desvanece hacia la izquierda y
     alcanza su máxima intensidad en el borde vivo, con un punto en la muestra
     más reciente y un relleno suave debajo.
   - Eje vertical autoescalado y suavizado (sin unidades): la señal ya llega
     filtrada y con el pico sistólico hacia arriba.
   - Sin señal (en espera, sin dedo, sin conexión) la traza se reemplaza por una
     línea punteada y un mensaje centrado.

**Estados del valor:** sin dato → guion "—" en `--color-muted`; lectura inválida
aislada → se conserva el último valor válido hasta 5 s ("mostrar el último
estado conocido, no vaciar la pantalla"); el pill dice "Calibrando…" si pasan
más de 5 s sin lectura válida.

**Modo demo:** debajo del titular aparece un botón suave "Simular botón de
medición" / "Detener medición (demo)". En producción no existe: la medición la
inicia el botón físico del dispositivo.

## Historial

- Cabecera: h1 "Historial" y a la derecha el conteo de mediciones.
- **Tarjeta de tendencia:** segmented "SpO₂ | Frecuencia" (una magnitud por vez,
  nunca dos ejes), gráfico de línea con área degradada, bandas de zona
  (rojo / ámbar / verde según umbrales) y punto final; ticks del eje Y en los
  umbrales y cinco rótulos de tiempo repartidos por igual. Debajo, segmented de
  rango "2 min | 10 min | 1 h" en tinta.
- **Lecturas:** título de sección, chips `Todas | Precaución | Peligro` y una
  lista **agrupada por medición**. Cada grupo lleva "Hoy · 14:32" y "Medición
  de 1 min 20 s" ("En curso" si sigue abierta). Cada fila: ícono de estado en
  cuadrado tintado (40 px), "97 % SpO₂ · 72 lpm", la etiqueta del estado y la
  hora (HH:MM:SS) a la derecha. El historial anterior al agrupamiento aparece
  como "Lecturas anteriores".
- Vacío: ícono + "Todavía no hay mediciones. Iniciá una desde el dashboard." (o
  "No hay lecturas con este filtro.").

## Ajustes

Grupos en tarjetas con título en mayúsculas pequeñas: **Conexión** (dirección
WebSocket, botón "Conectar", switch "Modo demo", datos del dispositivo),
**Apariencia** (segmented **Claro** | Oscuro | Auto; Claro es el predeterminado),
**Umbrales de SpO₂**, **Umbrales de frecuencia** (campos numéricos de 52 px en
grilla de 2 columnas; sin sliders: en una lectura de salud importa la
precisión), **Historial** ("Borrar historial", destructivo, con confirmación en
un segundo toque) y el aviso de que la app no es un dispositivo médico.

## Anti-patrones específicos de estas pantallas

- ❌ Mostrar lecturas o traza PPG antes de recibir el paquete de inicio.
- ❌ Dibujar la traza PPG con el dedo fuera del sensor.
- ❌ Procesar o filtrar la señal PPG en el cliente (llega acondicionada).
- ❌ Un eje doble SpO₂ + frecuencia.
- ❌ Estado por color solo (siempre ícono + texto).
- ❌ Vaciar el valor por una única lectura inválida.
