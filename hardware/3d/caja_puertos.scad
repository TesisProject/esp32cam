// Caja compacta para ESP32-CAM solar (sin placa perforada: modulos pegados al piso)
// Frente: jack DC 5,5x2,1 para PANEL y PILAS + pasacables para el cable de la CAMARA.
// Atras: hueco para el USB del TP4056 e interruptor rocker KCD11.
// Imprimir base y tapa por separado, sin soportes.

part = "base";            // [base, tapa, ambos]

// --- Interior ---
int_x = 72;               // Largo interior (mm)
int_y = 46;               // Fondo interior (mm)
alto_int = 25;            // Alto interior (mm)

// --- Caja ---
pared = 2.4;              // [1.6:0.4:4] Grosor de pared
piso = 2;                 // Grosor del piso
radio = 3;                // Radio de esquinas exteriores

// --- Frente ---
diametros = [8.2, 8.2, 7];   // Jack DC chico = 8.2 (grande = 11.2) ; pasacables camara = 7
puerto_z = 14;            // Altura del centro de los puertos desde la base
etiquetas = ["PANEL", "PILAS", "CAMARA"];
amarre = true;            // Anclaje interior para precinto del cable de la camara
amarre_idx = 2;           // Puerto (0, 1, 2) detras del cual va el anclaje

// --- Atras ---
usb_ancho = 13;           // Sirve para micro-USB y USB-C
usb_alto = 9;
usb_x = 14;               // Centro del USB desde el borde izquierdo (vista trasera)
usb_z = 7;                // Altura del centro del USB (TP4056 pegado al piso)
sw_ancho = 13.2;          // KCD11
sw_alto = 8.8;
sw_x = 54;                // Centro del interruptor desde el borde izquierdo (vista trasera)
sw_z = 14;

// --- Tapa ---
tapa_grosor = 2;
labio_alto = 4;
labio_grosor = 1.6;
tolerancia = 0.3;

$fn = 48;

ox = int_x + 2*pared;
oy = int_y + 2*pared;
oz = piso + alto_int;

module caja_redondeada(x, y, z, r) {
    hull() for (px = [r, x - r], py = [r, y - r])
        translate([px, py, 0]) cylinder(h = z, r = r);
}

module rect_redondeado(w, h, r = 1.2) {
    offset(r = r) square([w - 2*r, h - 2*r], center = true);
}

module texto(s, t = 3.6) {
    linear_extrude(0.6) text(s, size = t, halign = "center", valign = "center",
                             font = "Liberation Sans:style=Bold");
}

module base() {
    n = len(etiquetas);
    difference() {
        caja_redondeada(ox, oy, oz, radio);
        translate([pared, pared, piso]) caja_redondeada(int_x, int_y, alto_int + 1, max(radio - pared, 0.5));
        for (i = [0 : n - 1])
            translate([ox * (i + 1) / (n + 1), -1, puerto_z])
                rotate([-90, 0, 0]) cylinder(h = pared + 2, d = diametros[i]);
        translate([ox - usb_x, oy + 1, usb_z]) rotate([90, 0, 0])
            linear_extrude(pared + 2) rect_redondeado(usb_ancho, usb_alto);
        translate([ox - sw_x, oy + 1, sw_z]) rotate([90, 0, 0])
            linear_extrude(pared + 2) rect_redondeado(sw_ancho, sw_alto, 0.6);
    }
    if (amarre)
        translate([ox * (amarre_idx + 1) / (n + 1), pared + 8, piso - 0.01])
            difference() {
                translate([-6, -3, 0]) cube([12, 6, 6]);
                translate([-7, -2, 1.5]) cube([14, 4, 2.2]);
            }
    for (i = [0 : n - 1])
        translate([ox * (i + 1) / (n + 1), 0.01, puerto_z - diametros[i]/2 - 4])
            rotate([90, 0, 0]) texto(etiquetas[i], 3);
    translate([ox - usb_x, oy - 0.01, usb_z + usb_alto/2 + 4]) rotate([90, 0, 180]) texto("USB", 3.2);
    translate([ox - sw_x, oy - 0.01, sw_z + sw_alto/2 + 4]) rotate([90, 0, 180]) texto("ON / OFF", 3.2);
}

module tapa() {
    caja_redondeada(ox, oy, tapa_grosor, radio);
    translate([pared + tolerancia, pared + tolerancia, tapa_grosor - 0.01])
        difference() {
            caja_redondeada(int_x - 2*tolerancia, int_y - 2*tolerancia, labio_alto, max(radio - pared, 0.5));
            translate([labio_grosor, labio_grosor, -1])
                caja_redondeada(int_x - 2*tolerancia - 2*labio_grosor, int_y - 2*tolerancia - 2*labio_grosor,
                                labio_alto + 2, max(radio - pared - labio_grosor, 0.3));
        }
}

if (part == "base") base();
else if (part == "tapa") tapa();
else { base(); translate([0, oy + 15, 0]) tapa(); }
