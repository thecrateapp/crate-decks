# Traer arreglos de Mixxx

Decks es un fork duro de Mixxx (Diego, 2026-11-19): no sigue a upstream, no hace merges de sus tags ni de sus ramas y nada espera a una versión de Mixxx. Lo único que se trae son arreglos concretos, con cherry-pick, cuando resuelven algo nuestro: motor, audio, controladoras, seguridad o un fallo que nos afecta. Lo decide Diego, y nada vuelve a Mixxx (AGENTS.md §9).

## Preparación (una vez por clon)

```shell
git remote add upstream https://github.com/mixxxdj/mixxx.git   # si no existe
git remote set-url --push upstream DISABLED-never-push-to-mixxx
```

El push a `upstream` queda desactivado: un `git push upstream` falla en vez de publicar en Mixxx.

## Pasos

1. **Elegir el arreglo:** el commit o los commits de upstream que lo resuelven. Mirar antes qué ficheros tocan y cruzarlos con [CRATE_PATCHES.md](../CRATE_PATCHES.md). Si el arreglo depende de otros cambios de upstream que no tenemos, valorar si compensa traerlos o rehacerlo aquí.
2. **Leer upstream:**

   ```shell
   git fetch upstream
   ```

3. **Rama propia** desde `main`, con `-x` para que cada commit diga de dónde viene:

   ```shell
   git switch -c fix/mixxx-<tema> main
   git cherry-pick -x <commit>...
   ```

4. **Conflictos:** los de ficheros de [CRATE_PATCHES.md](../CRATE_PATCHES.md) los revisa Diego. Un workflow que borramos y Mixxx cambia se resuelve dejándolo borrado. Si el arreglo choca con la marca (textos que dicen "Mixxx"), pasa por la capa de marca como cualquier otro cambio.
5. **Compilar y probar** con los checks de siempre (`make checks`, `make test-unit` del área tocada y la suite completa al final). Se comparan los fallos con los de `main`: solo cuentan los nuevos.
6. **Prueba humana:** si el arreglo toca audio, motor, AutoDJ o controladoras, Diego lo prueba antes del merge (AGENTS.md §2).
7. **PR a `main`** con los commits de upstream que trae, por qué, los conflictos resueltos y el resultado de los tests. Si cambia algún parche, se actualiza [CRATE_PATCHES.md](../CRATE_PATCHES.md) en el mismo PR.

Si upstream cambia su `AGENTS.md`, no se copia: este repositorio tiene el suyo.
