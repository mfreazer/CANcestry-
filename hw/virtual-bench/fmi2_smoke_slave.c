/*
 * CANcestry T2 - smoke-test FMI 2.0 CoSimulation slave (H-08, issue #55).
 *
 * A minimal, dependency-free FMU executable used by fmi_bridge_test.py to
 * exercise the pinned FMPy 0.3.24 executor path (extract -> read_model_
 * description -> instantiate_fmu -> setupExperiment -> initialization
 * mode -> doStep -> getReal -> terminate) on any infrastructure with a
 * C compiler, WITHOUT OpenModelica. Bring-up finding F-19 (issue #55):
 * the bridge's FMPy usage must be verifiable against the exact pinned
 * version before each live dispatch, because the sandbox and the CI
 * image are the only places the pinned fmpy is importable.
 *
 * The model is a linear holdup discharge
 *
 *     vBat(t) = V0 - SLOPE_V_PER_US * t_us        (VR 0, output)
 *     t       = t_us / 1e6                        (VR 1, local)
 *
 * with V0 = 3.3 V and SLOPE_V_PER_US = 20e-6 (20 mV per ms), mirroring
 * the SHAPE of the CancestryLib.Power.Holdup energy balance. It is NOT
 * the production model: the tests only need a deterministic, strictly
 * decreasing output to prove the executor contract.
 *
 * Every FMI 2.0 CoSimulation entry point is exported. The optional
 * (partial) state/derivative functions are present because FMPy 0.3.24
 * requires their symbols when constructing FMU2Slave; they answer
 * fmi2Error and are never invoked by the bridge.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define SMOKE_V0          3.3
#define SMOKE_SLOPE_PER_US 20e-6

/* ------------------------------------------------------------------ */
/* FMI 2.0 typedefs (fmi2.h, inlined so the fixture needs no headers) */
/* ------------------------------------------------------------------ */

typedef int32_t  fmi2Boolean;
typedef int32_t  fmi2Int;
typedef int32_t  fmi2Integer;
typedef double   fmi2Real;
typedef unsigned char fmi2Byte;
typedef unsigned long fmi2ValueReference;
typedef void*    fmi2Component;
typedef void*    fmi2ComponentEnvironment;
typedef void*    fmi2InstanceToken;
typedef void*    fmi2FMUstate;
typedef const char* fmi2String;

typedef enum {
    fmi2OK = 0, fmi2Warning, fmi2Discard, fmi2Error, fmi2Fatal, fmi2Pending
} fmi2Status;

typedef enum { fmi2ModelExchange = 0, fmi2CoSimulation } fmi2Type;

typedef enum { fmi2DoStepStatus = 0, fmi2PendingStatus } fmi2StatusKind;

typedef void (*fmi2CallbackLogger)(fmi2InstanceToken, fmi2Status,
                                   const char*, const char*, ...);
typedef void* (*fmi2CallbackAllocateMemory)(size_t, size_t);
typedef void  (*fmi2CallbackFreeMemory)(void*);
typedef void  (*fmi2StepFinished)(fmi2ComponentEnvironment);

struct fmi2CallbackFunctions {
    fmi2CallbackLogger logger;
    fmi2CallbackAllocateMemory allocateMemory;
    fmi2CallbackFreeMemory freeMemory;
    void* stream;
    fmi2StepFinished stepFinished;
};
typedef struct fmi2CallbackFunctions fmi2CallbackFunctions;

/* ------------------------------------------------------------------ */
/* Component state                                                     */
/* ------------------------------------------------------------------ */

struct smoke_component {
    fmi2Real t_us;    /* accumulated simulation time, microseconds */
    int      valid;   /* component usable */
};

static fmi2Real smoke_vbat(const struct smoke_component *c)
{
    return SMOKE_V0 - SMOKE_SLOPE_PER_US * c->t_us;
}

/* ------------------------------------------------------------------ */
/* Version / instantiation                                             */
/* ------------------------------------------------------------------ */

fmi2String fmi2GetTypesPlatform(fmi2String* typesPlatform)
{
    *typesPlatform = "64";
    return "64";
}

fmi2String fmi2GetVersion(fmi2String* version)
{
    *version = "2.0";
    return "2.0";
}

fmi2Status fmi2SetDebugLogging(fmi2Component c, fmi2Boolean loggingOn,
                               size_t nCategories, const fmi2String categories)
{
    (void)c; (void)loggingOn; (void)nCategories; (void)categories;
    return fmi2OK;
}

fmi2Component fmi2Instantiate(fmi2String instanceName, fmi2Type fmuType,
                              fmi2String fmuGUID,
                              fmi2String fmuResourceLocation,
                              const fmi2CallbackFunctions* functions,
                              fmi2Boolean visible, fmi2Boolean loggingOn)
{
    (void)instanceName; (void)fmuGUID; (void)fmuResourceLocation;
    (void)functions; (void)visible; (void)loggingOn;
    if (fmuType != fmi2CoSimulation) {
        return NULL;  /* this fixture is a CoSimulation slave */
    }
    struct smoke_component *c = calloc(1, sizeof(struct smoke_component));
    if (c == NULL) {
        return NULL;
    }
    c->t_us = 0.0;
    c->valid = 1;
    return (fmi2Component)c;
}

void fmi2FreeInstance(fmi2Component c)
{
    free(c);
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

fmi2Status fmi2SetupExperiment(fmi2Component c, fmi2Boolean toleranceDefined,
                               fmi2Real tolerance, fmi2Real startTime,
                               fmi2Boolean stopTimeDefined, fmi2Real stopTime)
{
    (void)c; (void)toleranceDefined; (void)tolerance; (void)startTime;
    (void)stopTimeDefined; (void)stopTime;
    return fmi2OK;
}

fmi2Status fmi2EnterInitializationMode(fmi2Component c)
{
    (void)c;
    return fmi2OK;
}

fmi2Status fmi2ExitInitializationMode(fmi2Component c)
{
    (void)c;
    return fmi2OK;
}

fmi2Status fmi2Terminate(fmi2Component c)
{
    if (c != NULL) {
        ((struct smoke_component *)c)->valid = 0;
    }
    return fmi2OK;
}

fmi2Status fmi2Reset(fmi2Component c)
{
    if (c != NULL) {
        struct smoke_component *s = (struct smoke_component *)c;
        s->t_us = 0.0;
        s->valid = 1;
    }
    return fmi2OK;
}

/* ------------------------------------------------------------------ */
/* Variable access                                                     */
/* ------------------------------------------------------------------ */

fmi2Status fmi2GetReal(fmi2Component c, const fmi2ValueReference vr[],
                       size_t nvr, fmi2Real value[])
{
    const struct smoke_component *s = (const struct smoke_component *)c;
    size_t i;
    if (s == NULL || !s->valid) {
        return fmi2Error;
    }
    for (i = 0; i < nvr; ++i) {
        if (vr[i] == 0u) {
            value[i] = smoke_vbat(s);
        } else if (vr[i] == 1u) {
            value[i] = s->t_us * 1e-6;
        } else {
            return fmi2Error;
        }
    }
    return fmi2OK;
}

fmi2Status fmi2GetInteger(fmi2Component c, const fmi2ValueReference vr[],
                          size_t nvr, fmi2Int value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2Error;
}

fmi2Status fmi2GetBoolean(fmi2Component c, const fmi2ValueReference vr[],
                          size_t nvr, fmi2Boolean value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2Error;
}

fmi2Status fmi2GetString(fmi2Component c, const fmi2ValueReference vr[],
                         size_t nvr, fmi2String value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2Error;
}

fmi2Status fmi2SetReal(fmi2Component c, const fmi2ValueReference vr[],
                       size_t nvr, const fmi2Real value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2OK;
}

fmi2Status fmi2SetInteger(fmi2Component c, const fmi2ValueReference vr[],
                          size_t nvr, const fmi2Int value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2OK;
}

fmi2Status fmi2SetBoolean(fmi2Component c, const fmi2ValueReference vr[],
                          size_t nvr, const fmi2Boolean value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2OK;
}

fmi2Status fmi2SetString(fmi2Component c, const fmi2ValueReference vr[],
                         size_t nvr, const fmi2String value[])
{
    (void)c; (void)vr; (void)nvr; (void)value;
    return fmi2OK;
}

/* ------------------------------------------------------------------ */
/* CoSimulation stepping                                               */
/* ------------------------------------------------------------------ */

fmi2Status fmi2DoStep(fmi2Component c, fmi2Real currentCommunicationPoint,
                      fmi2Real communicationStepSize,
                      fmi2Boolean noSetFMUStatePriorToCurrentPoint)
{
    struct smoke_component *s = (struct smoke_component *)c;
    (void)currentCommunicationPoint;
    (void)noSetFMUStatePriorToCurrentPoint;
    if (s == NULL || !s->valid) {
        return fmi2Error;
    }
    if (communicationStepSize <= 0.0) {
        return fmi2Error;
    }
    s->t_us += communicationStepSize * 1e6;
    return fmi2OK;
}

fmi2Status fmi2CancelStep(fmi2Component c)
{
    (void)c;
    return fmi2OK;
}

fmi2Status fmi2GetStatus(fmi2Component c, fmi2StatusKind s, fmi2Status value[])
{
    (void)c; (void)s;
    *value = fmi2OK;
    return fmi2OK;
}

fmi2Status fmi2GetRealStatus(fmi2Component c, fmi2StatusKind s, fmi2Real value[])
{
    (void)c; (void)s;
    *value = 0.0;
    return fmi2OK;
}

fmi2Status fmi2GetIntegerStatus(fmi2Component c, fmi2StatusKind s, fmi2Int value[])
{
    (void)c; (void)s;
    *value = 0;
    return fmi2OK;
}

fmi2Status fmi2GetBooleanStatus(fmi2Component c, fmi2StatusKind s,
                                fmi2Boolean value[])
{
    (void)c; (void)s;
    *value = 0;
    return fmi2OK;
}

fmi2Status fmi2GetStringStatus(fmi2Component c, fmi2StatusKind s,
                               fmi2String value[])
{
    (void)c; (void)s;
    *value = "";
    return fmi2OK;
}

/* ------------------------------------------------------------------ */
/* Partial (optional) functions: present for symbol completeness,      */
/* never invoked by the T2 bridge.                                     */
/* ------------------------------------------------------------------ */

fmi2Status fmi2GetFMUstate(fmi2Component c, fmi2FMUstate s)
{
    (void)c; (void)s;
    return fmi2Error;
}

fmi2Status fmi2SetFMUstate(fmi2Component c, fmi2FMUstate s)
{
    (void)c; (void)s;
    return fmi2Error;
}

fmi2Status fmi2FreeFMUstate(fmi2Component c, fmi2FMUstate s)
{
    (void)c; (void)s;
    return fmi2Error;
}

fmi2Status fmi2SerializedFMUstateSize(fmi2Component c, fmi2FMUstate s, size_t size)
{
    (void)c; (void)s; (void)size;
    return fmi2Error;
}

fmi2Status fmi2SerializeFMUstate(fmi2Component c, fmi2FMUstate s,
                                 fmi2Byte serializedState[], size_t size)
{
    (void)c; (void)s; (void)serializedState; (void)size;
    return fmi2Error;
}

fmi2Status fmi2DeSerializeFMUstate(fmi2Component c, const fmi2Byte serializedState[],
                                   size_t size, fmi2FMUstate s)
{
    (void)c; (void)serializedState; (void)size; (void)s;
    return fmi2Error;
}

fmi2Status fmi2GetDirectionalDerivative(
    fmi2Component c, const fmi2ValueReference vrUnknown[], size_t nUnknown,
    const fmi2ValueReference vrKnown[], size_t nKnown,
    const fmi2Real dvKnown[], fmi2Real edvUnknown[])
{
    (void)c; (void)vrUnknown; (void)nUnknown; (void)vrKnown; (void)nKnown;
    (void)dvKnown; (void)edvUnknown;
    return fmi2Error;
}

fmi2Status fmi2EnterEventMode(fmi2Component c)
{
    (void)c;
    return fmi2Error;
}

fmi2Status fmi2NewDiscreteStates(fmi2Component c)
{
    (void)c;
    return fmi2Error;
}

fmi2Status fmi2EnterContinuousTimeMode(fmi2Component c)
{
    (void)c;
    return fmi2Error;
}

fmi2Status fmi2CompletedIntegratorStep(fmi2Component c,
                                       fmi2Boolean noSetFMUStatePriorToCurrentPoint)
{
    (void)c; (void)noSetFMUStatePriorToCurrentPoint;
    return fmi2Error;
}

fmi2Status fmi2SetTime(fmi2Component c, fmi2Real time)
{
    (void)c; (void)time;
    return fmi2Error;
}

fmi2Status fmi2SetContinuousStates(fmi2Component c, const fmi2Real x[], size_t nx)
{
    (void)c; (void)x; (void)nx;
    return fmi2Error;
}

fmi2Status fmi2GetDerivatives(fmi2Component c,
                              const fmi2ValueReference vrUnknown[], size_t nUnknown,
                              fmi2Real derUnknown[])
{
    (void)c; (void)vrUnknown; (void)nUnknown; (void)derUnknown;
    return fmi2Error;
}

fmi2Status fmi2GetEventIndicators(fmi2Component c,
                                  const fmi2ValueReference vrUnknown[], size_t nUnknown,
                                  fmi2Real derUnknown[])
{
    (void)c; (void)vrUnknown; (void)nUnknown; (void)derUnknown;
    return fmi2Error;
}

fmi2Status fmi2GetContinuousStates(fmi2Component c, fmi2Real x[])
{
    (void)c; (void)x;
    return fmi2Error;
}

fmi2Status fmi2GetNominalsOfContinuousStates(fmi2Component c, fmi2Real x_nominal[])
{
    (void)c; (void)x_nominal;
    return fmi2Error;
}

fmi2Status fmi2SetRealInputDerivatives(fmi2Component c,
                                       const fmi2ValueReference vr[], size_t nvr,
                                       fmi2Integer order[], const fmi2Real value[])
{
    (void)c; (void)vr; (void)nvr; (void)order; (void)value;
    return fmi2Error;
}

fmi2Status fmi2GetRealOutputDerivatives(fmi2Component c,
                                        const fmi2ValueReference vr[], size_t nvr,
                                        fmi2Integer order[], fmi2Real value[])
{
    (void)c; (void)vr; (void)nvr; (void)order; (void)value;
    return fmi2Error;
}
