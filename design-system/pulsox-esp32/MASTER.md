# Design System — PulsOx ESP32

> **LOGIC:** When building a specific screen, first check
> `design-system/pulsox-esp32/pages/[page-name].md`. If that file exists, its
> rules **override** this Master file. If not, follow the rules below.
>
> Rediseño "consumer wearable" (sucede al sistema teal/clínico anterior). Los
> tokens vivos están en `web/css/tokens.css`; este documento es la referencia
> de diseño detrás de ellos.

**Project:** PulsOx ESP32
**Dirección:** wearable de consumo (Apple Watch / Fitbit / Whoop): cálido,
legible, redondeado. **No** estética clínica/hospitalaria.
**Tema por defecto:** claro (día). Oscuro (noche) y Auto como opciones.

---

## Color

Dos colores de marca, cada uno ligado a una magnitud. Verde / ámbar / rojo
quedan **reservados para el estado fisiológico** y nunca se usan como decoración.

| Rol | Claro | Oscuro | Token |
|---|---|---|---|
| Fondo | `#F4F2EF` | `#0C0D12` | `--color-background` |
| Superficie (tarjetas, nav) | `#FFFFFF` | `#16181F` | `--color-surface` |
| Superficie 2 (controles) | `#ECE9E4` | `#22252F` | `--color-surface-2` |
| Pista (gauge, barras) | `#E6E3DE` | `#2A2D3A` | `--color-track` |
| Texto | `#16151D` | `#F5F4F8` | `--color-foreground` |
| Texto secundario | `#5B5A6B` | `#9EA0B4` | `--color-muted` |
| **SpO₂ / marca** (azul) | `#2B58E0` | `#6B97FF` | `--color-primary` |
| **Frecuencia / PPG** (violeta) | `#6A44D8` | `#B39BFF` | `--color-violet` |
| Destructivo | `#B93030` | `#E35B5B` | `--color-destructive` |

Contraste: texto secundario ≥ 4,5:1 sobre fondo y superficie en ambos temas; el
azul y el violeta de marca ≥ 4,5:1 sobre su superficie.

### Estado (fijo, validado, nunca tematizado)

| Estado | Valor | Token |
|---|---|---|
| Normal | `#0CA30C` | `--color-status-good` |
| Precaución | `#FAB219` | `--color-status-warning` |
| Peligro | `#D03B3B` claro / `#E35B5B` oscuro | `--color-status-critical` |

**Regla:** el estado nunca va solo en color. Siempre lleva ícono (check /
triángulo / octógono) **y** texto ("Normal" / "Precaución" / "Peligro"). El
ámbar tiene bajo contraste sobre claro, así que se usa como tinte de fondo o
color de ícono/anillo, y el texto del pill va en `--color-foreground`. Cada
estado trae un `tint` (fondo de pills y de íconos) y un `glow` (halo del anillo).

## Tipografía

**Figtree** (pesos 500 / 600 / 700 / 800) para todo. Cifras con
`font-variant-numeric: tabular-nums`.

| Uso | Tamaño / peso |
|---|---|
| Valor SpO₂ (anillo) | 66 px / 800 |
| Valor frecuencia | 52 px / 800 |
| Titular de pantalla (h1 de vista) | 30 px / 800 |
| Titular de estado (dashboard) | 26 px / 800 |
| Título de tarjeta | 16 px / 700 |
| Cuerpo / subtítulo | 14 px / 500 |
| Etiquetas, pills, controles | 12–14 px / 600–700 |

Un valor sin dato se muestra como guion "—" en `--color-muted`, peso 500 (nunca
en negrita: el guion pesado se lee como una barra).

## Forma y espaciado

- Radios: tarjetas 28 px (lista de historial 24 px), controles 16 px, botones
  grandes 18 px, **pills 999 px**.
- Espaciado entre bloques 12 px (24 px entre secciones); columna única de
  máx. 520 px con gutter de 20 px.
- Sin sombras en tarjetas: la jerarquía sale del contraste fondo/superficie. La
  única sombra es la del toast.

## Componentes

- **Pill de estado:** ícono + etiqueta, fondo = `tint` del estado, texto en
  `--color-foreground`.
- **Segmented control:** pista `--color-surface-2`, opción activa en
  `--color-primary` (selector de magnitud, tema) o en tinta (rango de tiempo).
- **Chips de filtro:** pill; activo en tinta (`--color-foreground`).
- **Switch:** 52 × 32 px, activo en `--color-primary`.
- **Campos:** 52 px de alto, fondo `--color-surface-2`, anillo de foco en
  `--color-primary`.
- **Barra de navegación inferior:** ver `pages/dashboard.md`.

## Movimiento

Transiciones de 150–400 ms con `cubic-bezier(0.4, 0, 0.2, 1)` (el gauge anima
su arco en 400 ms). `prefers-reduced-motion` desactiva transiciones y
animaciones decorativas; la traza PPG sigue desplazándose porque *es* el dato.

## Anti-patrones

- ❌ Emojis como íconos (usar SVG de trazo).
- ❌ Estado transmitido solo por color.
- ❌ Usar verde / ámbar / rojo como color decorativo o de marca.
- ❌ Animación tipo contador/odómetro en el valor principal (un signo vital en
  vivo no "carga" hasta su valor).
- ❌ Borrar o truncar el historial sin una acción explícita del usuario.
- ❌ Bloquear la UI o mostrar un spinner al reconectar: se mantiene la última
  lectura y el estado de conexión refleja el corte.
- ❌ Eje doble combinando SpO₂ y frecuencia en un mismo gráfico.
- ❌ Botones y campos de acción principal menores a 44 px (los chips y segmentados
  compactos miden 32–40 px y van siempre separados entre sí).

## Checklist previo a entregar

- [ ] Sin emojis; íconos de un mismo set de trazo
- [ ] `cursor: pointer` en todo lo clicable; foco visible (`:focus-visible`)
- [ ] Contraste de texto ≥ 4,5:1 en claro y oscuro
- [ ] `prefers-reduced-motion` respetado
- [ ] Sin scroll horizontal a 360 px, 390 px y escritorio
- [ ] Nada tapado por la barra de navegación fija (padding inferior + safe-area)
- [ ] Estados cubiertos: sin conexión, esperando medición, midiendo, sin dedo,
      calibrando, precaución, peligro
