/* Diagnostic: the floating-point operations the test synth and typical plugins use.
 * Writes c:\vstpoc-out\fpu.txt so the browser test can read it back. */
#include <math.h>
#include <stdio.h>
#include <windows.h>

static volatile float vf = 0.5f;
static volatile double vd = 0.5;

int main(void) {
    CreateDirectoryA("c:\\vstpoc-out", NULL);
    FILE *f = fopen("c:\\vstpoc-out\\fpu.txt", "wb");
    if (!f) return 1;
    float x = vf;
    double y = vd;
    fprintf(f, "expf(-0.5)=%.6f\n", (double)expf(-x));
    fprintf(f, "powf(2,0.5)=%.6f\n", (double)powf(2.0f, x));
    fprintf(f, "powf(250,0.6)=%.6f\n", (double)powf(250.0f, x + 0.1f));
    fprintf(f, "exp(-0.5)=%.6f\n", exp(-y));
    fprintf(f, "pow(2,0.5)=%.6f\n", pow(2.0, y));
    fprintf(f, "log(0.5)=%.6f\n", log(y));
    fprintf(f, "sin(0.5)=%.6f\n", sin(y));
    fprintf(f, "sqrt(0.5)=%.6f\n", sqrt(y));
    fprintf(f, "tan(0.5)=%.6f\n", tan(y));
    fprintf(f, "mul=%.6f add=%.6f div=%.6f\n", (double)(x * 3.0f), (double)(x + 0.25f), (double)(x / 4.0f));
    float acc = 0;
    for (int i = 0; i < 1000; ++i) acc += 0.001f * (float)i;
    fprintf(f, "loop=%.6f\n", (double)acc);
    fclose(f);
    return 0;
}
