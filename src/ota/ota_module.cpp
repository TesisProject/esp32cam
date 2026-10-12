/* ============================================================================
 *  ota_module.cpp  -  Version, particion activa y rollback de la imagen
 *
 *  Como funciona el rollback (bootloader del core 3.3.12, compilado con
 *  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y):
 *    - Una imagen recien escrita por OTA arranca en estado NEW y el
 *      bootloader la pasa a PENDING_VERIFY en ese primer arranque.
 *    - Si la app no la marca como valida, en el SIGUIENTE arranque el
 *      bootloader la ve aun en PENDING_VERIFY, la marca ABORTED y arranca la
 *      otra particion (la version anterior).
 *    - Despertar del deep sleep ES un arranque completo (pasa por el
 *      bootloader). Asi que una imagen que se duerme sin validarse vuelve
 *      sola a la anterior al despertar. Esa es justo la red de seguridad que
 *      se quiere: una version que no sabe subir fotos se descarta sola.
 *
 *  Por defecto, initArduino() la marca valida en cuanto arranca (antes de
 *  setup()), lo que anula la proteccion: una version que arranca pero no
 *  conecta o no sube se quedaria para siempre. De ahi el override de
 *  verifyRollbackLater() y la validacion manual tras un ciclo bueno.
 *
 *  Imagen flasheada por USB: boot_app0.bin deja otadata con la ranura app0
 *  en estado UNDEFINED (0xFFFFFFFF), no PENDING_VERIFY. El bootloader nunca
 *  hace rollback de UNDEFINED y ota_confirm_if_pending() no la toca (ni
 *  escribe en flash), asi que el flujo normal de desarrollo no cambia.
 * ==========================================================================*/
#include "ota_module.h"
#include "../../config.h"

#include <esp_ota_ops.h>

/* El core declara este hook como weak en esp32-hal-misc.c, un fichero C y
 * sin prototipo en ningun header. Sin extern "C", esta definicion C++
 * saldria con nombre mangleado (_Z19verifyRollbackLaterv), no sustituiria a
 * la weak y el core seguiria validando la imagen al arrancar, sin ningun
 * error ni aviso. true = "no verifiques en initArduino(), lo hago yo". */
extern "C" bool verifyRollbackLater() { return true; }

static const char *state_name(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED (flasheo USB)";
    default:                         return "?";
  }
}

void ota_log_boot_info() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  const bool has_state =
      running && esp_ota_get_state_partition(running, &state) == ESP_OK;

  LOG("OTA: FW_VERSION=%d  particion activa=%s  estado=%s", FW_VERSION,
      running ? running->label : "?",
      has_state ? state_name(state) : "sin otadata");

  /* Si hubo rollback, la ranura descartada queda marcada como invalida (hasta
   * que un OTA la sobrescriba): es la forma de verlo desde el log, porque la
   * version que corre es la anterior y FW_VERSION no lo delata por si solo. */
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
  if (invalid) {
    LOG("OTA: la imagen de %s esta descartada (hubo rollback)", invalid->label);
  }
}

void ota_confirm_if_pending() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;

  /* Solo PENDING_VERIFY. Llamarlo en otro estado no rompe nada, pero con
   * UNDEFINED (flasheo USB) reescribiria otadata en flash sin necesidad. */
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    LOG("OTA: imagen de %s confirmada (ciclo completo con subida ok)",
        running->label);
  } else {
    /* Si falla, la imagen sigue pendiente y el proximo arranque volvera a la
     * anterior: preferible a quedarse con una version sin confirmar. */
    LOG("OTA: no se pudo confirmar la imagen (0x%x)", (unsigned)err);
  }
}
