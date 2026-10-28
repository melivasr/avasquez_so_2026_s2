# Cyclyc Redundancy Check de 16 bits

Su utilidad principal es verificar paquetes de datos en puertos USB, UART o I2C.

## 1. Generación: 
El emisor toma los datos originales y les aplica un cálculo matemático basado en una división de polinomios (aritmética binaria o de Galois).
## 2. Resultado: 
El cálculo produce un valor único de 16 bits (dos bytes) llamado "suma de verificación" o checksum, el cual se añade al final del mensaje.
## 3. Verificación: 
El receptor realiza exactamente el mismo cálculo con los datos recibidos y compara su resultado con el CRC que venía adjunto.
## 4. Resultado de la comparación:
```
• Si ambos coinciden, los datos están íntegros.
• Si no coinciden, significa que la información se corrompió durante el envío o la lectura.
```

## Características principales

- Tamaño: Produce un código de salida fijo de 16 bits.
- Capacidad de detección: Es muy confiable para bloques de datos pequeños o medianos; detecta todos los errores de un solo bit y la gran mayoría de errores en ráfaga de hasta 16 bits de longitud.
- Variantes: Existen diferentes estándares de CRC-16 según el polinomio generador que utilicen (como CRC-16-CCITT, CRC-16-IBM, Modbus, etc.).

## Configuración CCITT-FALSE

Define una configuración exacta y un conjunto de parámetros específicos para calcular el algoritmo de verificación CRC.

Usar el mismo polinomio no es suficiente; dos sistemas deben acordar exactamente cómo inicializar y procesar los bits para obtener el mismo resultado.

### 1. Polinomio `0x1021` (UIT-T)

El polinomio es la "clave" matemática (el divisor binario) que define cómo se mezclan los bits de los datos.

En CCCIT este el polinomio estándar (hoy conocida como UIT-T) y es uno de los más eficientes para detectar errores en ráfaga.

Algebráicamente:

```math
(X^{16} + X^{12} + X^5 + 1)
```

En binario:
``` 
0001 0000 0010 0001 
``` 

### 2. Valor Inicial `0xFFFF`

El registro interno del CRC (la memoria donde se acumula el resultado) no se vacía a cero, sino que se llena por completo con unos (`1111 1111 1111 1111`).

Esto se hace así para diferenciar un mensaje que empiece con muchos bytes de ceros seguidos, pues no cambiaría el estado del CRC, haciendo imposible detectar si se perdieron o añadieron ceros al principio del mensaje. 

Inicializarlo en 0xFFFF obliga a que cualquier cero inicial altere inmediatamente el cálculo.

### 3. Sin XOR Final (XOR de salida `0x0000`)

Una vez que el algoritmo termina de procesar el último bit de los datos, se obtiene un resultado matemático.

En algunas variantes de CRC, ese resultado se pasa por una compuerta XOR con otro número (como `0xFFFF`) para invertir todos los bits antes de entregarlo.

"Sin XOR Final" significa que el valor se entrega tal cual sale del cálculo matemático, sin aplicar ninguna inversión ni máscara adicional (es equivalente a hacer un `XOR` con `0x0000`).

### 4. FALSE

Se refiere principalmente al orden de los bits (Reflect):

- No reflejado (False): 

Los datos se procesan empezando por el `bit más significativo (MSB)` de cada byte, y el resultado final también se mantiene en ese orden.

- True (o "XModem"):

Procesa o refleja los bits al revés (`empezando por el bit menos significativo o LSB`)