# Luna

Explorador de archivos para Linux escrito en C++ con Qt 6: una alternativa ligera a
Nautilus, pensada para el escritorio [Omarchy](https://omarchy.org) (usa los colores
de su tema y lanza las aplicaciones como él), pero que funciona en cualquier
escritorio. Lleva el nombre de Luna, una gata siamesa, que es también su icono.

![Icono de Luna](data/luna.svg)

## Qué hace

- **Vistas:** iconos, compacta, lista, lista detallada, árbol y columnas; pestañas y
  vista dividida en la misma ventana.
- **Barra de ruta** al estilo de Nautilus, como botones o como texto editable.
- **Barra lateral** con lugares, marcadores (los mismos que GTK y Nautilus),
  dispositivos extraíbles (montar y expulsar) y red.
- **Archivos:** abrir con (respeta `mimeapps.list`), mover a / copiar a, copiar y
  mover con progreso y gestión de conflictos, papelera estándar, renombrar en lote,
  comprimir y extraer, enlaces, deshacer y rehacer.
- **Propiedades:** permisos (casillas, octal, bits especiales, recursivo) y
  metadatos de audio y vídeo (libmediainfo) y de fotos, con EXIF y GPS (exiv2).
- **Conectar a un servidor:** SFTP/SSH, FTP, FTPS, SMB y NFS a través de GVFS.
- **Rápido y ligero:** listado en hilos aparte, búsqueda recursiva a prioridad
  mínima con `getdents64`, miniaturas en la caché compartida de freedesktop (las
  mismas que usa Nautilus) con vista previa EXIF al instante. No consume CPU en
  reposo.
- **Muy configurable:** barra superior (solo iconos, iconos y texto, o agrupados),
  tamaños, columnas, comportamiento al abrir… en *Ajustes*.
- Interfaz en español.

## Instalar desde el zip

El zip de cada [versión](https://github.com/julssvanderhaack/luna/releases) trae el
ejecutable ya compilado para x86-64. Está enlazado con las bibliotecas del sistema,
así que necesitas versiones al menos tan recientes como aquellas con las que se
compiló (**Qt 6.12**, exiv2 0.28, poppler 26): en la práctica, Omarchy o cualquier
distribución basada en Arch al día.

```sh
sudo pacman -S --needed qt6-base qt6-svg libmediainfo exiv2 poppler-qt6
# opcionales: miniaturas de vídeo, servidores remotos y comprimir/extraer
sudo pacman -S --needed ffmpegthumbnailer gvfs gvfs-smb libarchive 7zip
unzip luna-1.0.0-linux-x86_64.zip
cd luna-1.0.0-linux-x86_64
./install.sh            # en ~/.local (sin sudo)
# sudo ./install.sh --system   # para todos los usuarios, en /usr/local
```

`./install.sh --uninstall` (o `sudo ./install.sh --system --uninstall`) lo quita.

Luna no se pone como explorador predeterminado al instalarlo. Para hacerlo:

```sh
xdg-mime default luna.desktop inode/directory
```

En otras distribuciones, o si el ejecutable se queja de alguna biblioteca, compílalo.

## Compilar

Necesitas un compilador con C++17, CMake ≥ 3.20, Qt 6 (Widgets, Svg),
libmediainfo, exiv2 y poppler-qt6.

```sh
# Arch: sudo pacman -S --needed base-devel cmake qt6-base qt6-svg libmediainfo exiv2 poppler-qt6
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build --prefix ~/.local
```

Por defecto se optimiza para el procesador de la máquina que compila
(`-march=native`); `-DLUNA_NATIVE=OFF` genera un ejecutable para cualquier x86-64.

## Atajos de teclado

| Tecla | Acción |
|---|---|
| Alt + ← / → / ↑, Retroceso | Atrás / adelante / carpeta superior |
| Ctrl + L | Escribir una ruta |
| Ctrl + F o empezar a escribir | Filtrar (Intro busca en subcarpetas) |
| Ctrl + T / Ctrl + W, Ctrl + Tab | Pestañas |
| F3 / F6 | Vista dividida / cambiar de panel |
| F9 | Barra lateral |
| Ctrl + 1 … 6 | Iconos, compacta, lista, detallada, árbol, columnas |
| Ctrl + rueda, Ctrl + / − | Tamaño de los iconos |
| Ctrl + H | Mostrar archivos ocultos |
| F2 | Renombrar (en lote si hay varios) |
| Supr / Mayús + Supr | Papelera / eliminar definitivamente |
| Ctrl + C / X / V | Copiar / cortar / pegar |
| Ctrl + Z / Ctrl + Mayús + Z | Deshacer / rehacer |
| Ctrl + Mayús + N | Nueva carpeta |
| Alt + Intro | Propiedades |
| F4 | Terminal aquí |
| Ctrl + D | Añadir a marcadores |
| Ctrl + , | Ajustes |

Los ajustes se guardan en `~/.config/luna/luna.conf`.

## Licencia

[GPL-3.0](LICENSE).
