/**
 * Google Apps Script - API de Calendario para ESP32 e-Paper Display
 *
 * INSTRUCCIONES DE DESPLIEGUE:
 * 1. Ve a https://script.google.com y crea un nuevo proyecto.
 * 2. Pega este código, reemplazando CALENDAR_ID con el ID de tu calendario
 *    (lo encuentras en Google Calendar > Configuración del calendario > ID del calendario)
 * 3. Menú "Desplegar" > "Nueva implementación"
 *    - Tipo: Aplicación web
 *    - Ejecutar como: Yo (tu cuenta de Google)
 *    - Quién tiene acceso: Cualquier usuario (incluidos anónimos)
 * 4. Copia la URL generada (formato: https://script.google.com/macros/s/SCRIPT_ID/exec)
 * 5. Pega esa URL en GCAL_SCRIPT_URL dentro de platformio/src/config.cpp
 *
 * NOTA: Cada vez que modifiques el script, debes crear una NUEVA implementación
 * (no actualizar la existente) para que los cambios surtan efecto en el ESP32.
 */

var CALENDAR_ID = "family12173188443280700777@group.calendar.google.com";
var DAYS_AHEAD  = 14; // cuántos días hacia adelante buscar eventos
var MAX_EVENTS  = 10; // máximo de eventos a devolver

function doGet(e) {
  var now     = new Date();
  var endDate = new Date(now.getTime() + DAYS_AHEAD * 24 * 60 * 60 * 1000);

  var calendar = CalendarApp.getCalendarById(CALENDAR_ID);
  if (!calendar) {
    return ContentService
      .createTextOutput(JSON.stringify({error: "Calendar not found: " + CALENDAR_ID}))
      .setMimeType(ContentService.MimeType.JSON);
  }

  var calEvents = calendar.getEvents(now, endDate);
  var result    = [];

  var TZ = "Europe/Madrid";
  for (var i = 0; i < Math.min(calEvents.length, MAX_EVENTS); i++) {
    var ev = calEvents[i];
    result.push({
      start:   Utilities.formatDate(ev.getStartTime(), TZ, "yyyy-MM-dd'T'HH:mm:ss"),
      end:     Utilities.formatDate(ev.getEndTime(),   TZ, "yyyy-MM-dd'T'HH:mm:ss"),
      summary: ev.getTitle()
    });
  }

  return ContentService
    .createTextOutput(JSON.stringify({events: result}))
    .setMimeType(ContentService.MimeType.JSON);
}
