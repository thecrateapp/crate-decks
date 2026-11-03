# Traer cambios de Mixxx

Los cambios de Mixxx llegan en una sola dirección y cuando Diego lo decide: un merge de un tag estable o cherry-picks de arreglos (motor, audio, controladoras, seguridad). No hay sincronía automática, y nada vuelve a Mixxx (AGENTS.md §9).

## Preparación (una vez por clon)

```shell
git remote add upstream https://github.com/mixxxdj/mixxx.git   # si no existe
git remote set-url --push upstream DISABLED-never-push-to-mixxx
```

El push a `upstream` queda desactivado: un `git push upstream` falla en vez de publicar en Mixxx.

## Pasos

1. **Elegir qué se trae:** un tag (`2.7.0`) o los commits de un arreglo. Mirar antes qué ficheros toca y cruzarlos con [CRATE_PATCHES.md](../CRATE_PATCHES.md).
2. **Actualizar `upstream`** (solo con un tag o un commit de Mixxx sin cambios, nunca con código nuestro):

   ```shell
   git fetch upstream --tags
   git switch upstream
   git merge --ff-only <tag-o-commit>
   ```

3. **Rama de merge** desde `main`:

   ```shell
   git switch -c merge/mixxx-<ref> main
   git merge upstream            # un tag entero
   # o bien, para un arreglo suelto:
   git cherry-pick -x <commit>...
   ```

4. **Conflictos:** los de ficheros de [CRATE_PATCHES.md](../CRATE_PATCHES.md) los revisa Diego. Un workflow que borramos y Mixxx cambia se resuelve dejándolo borrado.
5. **Compilar y probar:**

   ```shell
   cmake --build build -j"$(nproc)"
   build/mixxx-test
   ```

   Se comparan los fallos con los de `main` antes del merge: solo cuentan los nuevos.
6. **Prueba humana:** si el cambio toca audio, motor, AutoDJ o controladoras, Diego lo prueba con un setup de DJ real antes del merge (AGENTS.md §2).
7. **PR a `main`** con la lista de lo que se trae, los conflictos resueltos y el resultado de los tests. Si cambia algún parche, se actualiza [CRATE_PATCHES.md](../CRATE_PATCHES.md) en el mismo PR.

Si upstream cambia su `AGENTS.md`, no se copia: este repositorio tiene el suyo.
