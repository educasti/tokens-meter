#pragma once
#include <lvgl.h>

// Command Center: overlay de dashboard del propio dispositivo (radios,
// version de firmware y estado OTA pull con progreso de descarga).
// Contrato: design/command-center/SPEC.md secciones 3-6 y 8-10.
//
// No es una pantalla del ciclo: es un contenedor opaco a pantalla completa,
// hijo topmost de la pantalla activa, que se abre con gesto (borde superior
// hacia abajo) y se cierra con gesto hacia arriba o toque. Los dots y la
// navegacion no cambian. El cableado (handlers de gesto/tap en ui.cpp y el
// `case` PWR en main.cpp) lo pone W3; este modulo solo expone la API de
// abajo y no toca ningun archivo existente.
//
// Reglas (SPEC seccion 9):
//   * sin condicionales por placa: la geometria sale de board_caps() en tiempo de
//     ejecucion, igual que ui_portfolio.cpp;
//   * lazy init: sys_init_lazy() no crea objetos LVGL (el heap LVGL ya va
//     ~48 KB ocupado); los widgets se construyen en el primer sys_show();
//   * sys_tick() lee todos los datos primero y solo despues toca LVGL, y
//     solo ante cambios (redraw-on-change contra el ultimo snapshot pintado).
void sys_init_lazy(void);   // barato, sin LVGL; llamar una vez desde ui_init
void sys_show(void);        // crea (lazy) + reparenta a lv_screen_active() + muestra
void sys_hide(void);        // oculta y desarma la confirmacion pendiente (no libera widgets)
bool sys_is_open(void);     // true con el overlay visible (main.cpp decide PWR)
void sys_tick(void);        // lectura en vivo + repintado; llamar desde ui_tick_anim()
void sys_pwr_action(void);  // PWR corto con overlay abierto (maquina SPEC seccion 5)
