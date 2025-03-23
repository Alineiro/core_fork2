#pragma once

#include "public_settings.h"

#include "contracts/qpi.h"

#include "platform/global_var.h"
#include "platform/read_write_lock.h"
#include "platform/debugging.h"
#include "platform/memory.h"

#include "contract_core/contract_def.h"
#include "contract_core/stack_buffer.h"
#include "contract_core/contract_action_tracker.h"

#include "logging/logging.h"
#include "common_buffers.h"

using namespace QPI;

struct Escrow
{
    address         sender;
    address         recipient;
    unsigned int    amount;
    bool            isLocked;
};

// ===================== NEW: Estructuras para Atomic Swap =====================
// Estructura auxiliar para almacenar los datos de un intercambio atómico activo
struct SwapInfo 
{
    address partyA;   ///< Dirección de la parte A (iniciador del swap)
    address partyB;   ///< Dirección de la parte B (contraparte designada)
    uint64 tokenA;    ///< Identificador del token que ofrece la parte A
    uint64 amountA;   ///< Cantidad del token que deposita la parte A
    uint64 tokenB;    ///< Identificador del token que ofrece la parte B (lo que A desea)
    uint64 amountB;   ///< Cantidad del token que deposita la parte B
    bool   active;    ///< Indicador de si hay un intercambio activo (true cuando el swap está en proceso)
};

struct HM25 : public ContractBase
{
public:

    // ===================== NEWS: Intercambio Atómico (Atomic Swap) =====================

    struct Deposit_input 
    {
        address counterparty;    ///< Dirección de la otra parte del intercambio (parte B)
        uint64 offeredToken;     ///< Token (identificador) que esta parte ofrece y va a depositar
        uint64 offeredAmount;    ///< Cantidad del token que deposita esta parte
        uint64 desiredToken;     ///< Token (identificador) que esta parte desea recibir a cambio
        uint64 desiredAmount;    ///< Cantidad del token que desea recibir
    };
    struct Deposit_output 
    {
        bool success;            ///< Indica si la operación (depósito e intercambio) se realizó con éxito
    };

    // ===================== OLDS =====================
    struct Echo_input{};
    struct Echo_output{};
    struct Burn_input{};
    struct Burn_output{};
    struct GetStats_input {};
    struct GetStats_output
    {
        uint64 numberOfEchoCalls;
        uint64 numberOfBurnCalls;
    };

private:
    uint64 numberOfEchoCalls;
    uint64 numberOfBurnCalls;

    Escrow   escrow;
    SwapInfo swap;    // Estado del intercambio atómico actual (solo uno activo a la vez)

public:

    /**
     * Procedimiento público Deposit: maneja depósitos de ambas partes y realiza el swap atómico.
     * - Si no hay un swap activo, este llamado registra la oferta de la parte A e inicia el escrow.
     * - Si ya hay un swap en proceso, este llamado corresponde a la parte B y completará el intercambio si todos los datos coinciden.
     */
    PUBLIC_PROCEDURE(Deposit)
        // Verificar si ya existe un intercambio activo en curso
        if (!state.swap.active)
        {
            // Rama para iniciar un nuevo intercambio (primera parte deposita)
            require(qpi.invocationReward() > 0, "No se envio deposito en la invocacion");
            require(input.counterparty != address::null(), "Contraparte no valida");

            // Registrar datos de la oferta de la parte A en el estado
            state.swap.partyA   = qpi.invocator();        // Quien invoca es parte A (iniciador)
            state.swap.partyB   = input.counterparty;     // Establecer la contraparte (parte B)
            state.swap.tokenA   = input.offeredToken;     // Token que ofrece A 
            state.swap.amountA  = input.offeredAmount;    // Cantidad del token ofrecido por A
            state.swap.tokenB   = input.desiredToken;     // Token que A espera recibir de B
            state.swap.amountB  = input.desiredAmount;    // Cantidad de token que A espera recibir

            // Bloquear el depósito de A en el contrato
            if (input.offeredToken == qpi.nativeToken())
            {
                // Si el token ofrecido es la moneda nativa, la cantidad enviada debe coincidir con lo declarado
                require(qpi.invocationReward() == state.swap.amountA, "El monto enviado no coincide con la oferta");
                // (Los fondos nativos enviados quedan retenidos en el contrato automáticamente)
            }
            else
            {
                // Si el token ofrecido es un token no nativo, verificar balance y transferirlo al contrato
                require(qpi.balanceOf(input.offeredToken, qpi.invocator()) >= state.swap.amountA, 
                        "Saldo insuficiente del token ofrecido");
                // Transferir del saldo de A al contrato la cantidad ofertada de dicho token
                qpi.transferFrom(qpi.invocator(), qpi.contractAddress(), input.offeredToken, state.swap.amountA);
            }

            state.swap.active = true;    // Marcar que hay un swap pendiente en curso
            output.success = true;       // Depósito de A realizado con éxito (swap pendiente)
        }
        else
        {
            // Rama para completar el intercambio (segunda parte deposita, parte B)
            require(state.swap.active, "No hay un intercambio activo");
            require(qpi.invocator() == state.swap.partyB, "Solo la contraparte designada puede completar el swap");

            // Verificar que la oferta de B coincide con lo que A espera
            require(input.offeredToken == state.swap.tokenB, "Token ofrecido por B no coincide con el esperado");
            require(input.offeredAmount == state.swap.amountB, "Cantidad ofrecida por B no coincide con la esperada");
            // Verificar que lo que B desea recibir coincide con lo que A ofrecio
            require(input.desiredToken == state.swap.tokenA, "Token deseado por B no coincide con el ofrecido por A");
            require(input.desiredAmount == state.swap.amountA, "Cantidad deseada por B no coincide con la ofrecida por A");

            // Bloquear el depósito de B en el contrato
            if (input.offeredToken == qpi.nativeToken())
            {
                // Si B ofrece moneda nativa, la cantidad enviada en la invocación debe ser la esperada
                require(qpi.invocationReward() == state.swap.amountB, "El monto enviado por B no coincide con su oferta");
                // (Fondos nativos de B quedan ahora retenidos en el contrato)
            }
            else
            {
                // Si B ofrece un token no nativo, verificar balance de B y transferir al contrato
                require(qpi.balanceOf(input.offeredToken, qpi.invocator()) >= state.swap.amountB, 
                        "Saldo insuficiente del token ofrecido por B");
                qpi.transferFrom(qpi.invocator(), qpi.contractAddress(), input.offeredToken, state.swap.amountB);
            }

            // Ambas partes depositaron: realizar intercambios cruzados de tokens
            if (state.swap.tokenA == qpi.nativeToken())
            {
                // Transferir moneda nativa depositada por A hacia B
                qpi.transfer(state.swap.partyB, state.swap.amountA);
            }
            else
            {
                // Transferir token A (depositado por A) desde el contrato hacia B
                qpi.transfer(state.swap.partyB, state.swap.tokenA, state.swap.amountA);
            }

            if (state.swap.tokenB == qpi.nativeToken())
            {
                // Transferir moneda nativa depositada por B hacia A
                qpi.transfer(state.swap.partyA, state.swap.amountB);
            }
            else
            {
                // Transferir token B (depositado por B) desde el contrato hacia A
                qpi.transfer(state.swap.partyA, state.swap.tokenB, state.swap.amountB);
            }

            state.swap.active = false;   // Marcar que el swap ha finalizado
            output.success = true;       // Intercambio completado con éxito
        }
    _

    // ===================== OLDS (funcionalidad previa) =====================

    PUBLIC_PROCEDURE(Echo)
        state.numberOfEchoCalls++;
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
    _

    PUBLIC_PROCEDURE(Burn)
        state.numberOfBurnCalls++;
        if (qpi.invocationReward() > 0)
        {
            qpi.burn(qpi.invocationReward());
        }
    _

    PUBLIC_FUNCTION(GetStats)
        output.numberOfBurnCalls = state.numberOfBurnCalls;
        output.numberOfEchoCalls = state.numberOfEchoCalls;
    _

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES

        REGISTER_USER_PROCEDURE(Deposit, 3);
        REGISTER_USER_PROCEDURE(Echo, 1);
        REGISTER_USER_PROCEDURE(Burn, 2);

        REGISTER_USER_FUNCTION(GetStats, 1);
    _

    INITIALIZE
        state.numberOfEchoCalls = 0;
        state.numberOfBurnCalls = 0;
        state.swap.active = false;   // Inicialmente no hay ningún swap activo
    _
};
