/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <utility>
#include "common/assert.h"
#include "frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {
namespace {
std::pair<size_t, Vec> Combine(Imm<2> size, Imm<1> H, Imm<1> L, Imm<1> M, Imm<4> Vmlo) {
    if (size == 0b01) {
        return {concatenate(H, L, M).ZeroExtend(), Vmlo.ZeroExtend<Vec>()};
    }

    return {concatenate(H, L).ZeroExtend(), concatenate(M, Vmlo).ZeroExtend<Vec>()};
}

enum class ExtraBehavior {
    None,
    Accumulate,
    Subtract,
};

bool MultiplyByElement(TranslatorVisitor& v, bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd,
                       ExtraBehavior extra_behavior) {
    if (size != 0b01 && size != 0b10) {
        return v.UnallocatedEncoding();
    }

    const auto [index, Vm] = Combine(size, H, L, M, Vmlo);
    const size_t idxdsize = H == 1 ? 128 : 64;
    const size_t esize = 8 << size.ZeroExtend();
    const size_t datasize = Q ? 128 : 64;
    
    const IR::U128 operand1 = v.V(datasize, Vn);
    const IR::U128 operand2 = v.ir.VectorBroadcast(esize, v.ir.VectorGetElement(esize, v.V(idxdsize, Vm), index));
    const IR::U128 operand3 = v.V(datasize, Vd);
    
    IR::U128 result = v.ir.VectorMultiply(esize, operand1, operand2);
    if (extra_behavior == ExtraBehavior::Accumulate) {
        result = v.ir.VectorAdd(esize, operand3, result);
    } else if (extra_behavior == ExtraBehavior::Subtract) {
        result = v.ir.VectorSub(esize, operand3, result);
    }

    v.V(datasize, Vd, result);
    return true;
}

bool FPMultiplyByElement(TranslatorVisitor& v, bool Q, bool sz, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd,
                         ExtraBehavior extra_behavior) {
    if (sz && L == 1) {
        return v.UnallocatedEncoding();
    }
    if (sz && !Q) {
        return v.ReservedValue();
    }

    const size_t idxdsize = H == 1 ? 128 : 64;
    const size_t index = sz ? H.ZeroExtend() : concatenate(H, L).ZeroExtend();
    const Vec Vm = concatenate(M, Vmlo).ZeroExtend<Vec>();
    const size_t esize = sz ? 64 : 32;
    const size_t datasize = Q ? 128 : 64;

    const IR::UAny element2 = v.ir.VectorGetElement(esize, v.V(idxdsize, Vm), index);
    const IR::U128 operand1 = v.V(datasize, Vn);
    const IR::U128 operand2 = Q ? v.ir.VectorBroadcast(esize, element2) : v.ir.VectorBroadcastLower(esize, element2);
    const IR::U128 operand3 = v.V(datasize, Vd);

    const IR::U128 result = [&]{
        switch (extra_behavior) {
        case ExtraBehavior::None:
            return v.ir.FPVectorMul(esize, operand1, operand2);
        case ExtraBehavior::Accumulate:
            return v.ir.FPVectorMulAdd(esize, operand3, operand1, operand2);
        case ExtraBehavior::Subtract:
            return v.ir.FPVectorMulAdd(esize, operand3, v.ir.FPVectorNeg(esize, operand1), operand2);
        }
        UNREACHABLE();
        return IR::U128{};
    }();
    v.V(datasize, Vd, result);
    return true;
}

using ExtensionFunction = IR::U32 (IREmitter::*)(const IR::UAny&);

bool DotProduct(TranslatorVisitor& v, bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H,
                Vec Vn, Vec Vd, ExtensionFunction extension) {
    if (size != 0b10) {
        return v.ReservedValue();
    }

    const Vec Vm = concatenate(M, Vmlo).ZeroExtend<Vec>();
    const size_t esize = 8 << size.ZeroExtend();
    const size_t datasize = Q ? 128 : 64;
    const size_t elements = datasize / esize;
    const size_t index = concatenate(H, L).ZeroExtend();

    const IR::U128 operand1 = v.V(datasize, Vn);
    const IR::U128 operand2 = v.V(128, Vm);
    IR::U128 result = v.V(datasize, Vd);

    for (size_t i = 0; i < elements; i++) {
        IR::U32 res_element = v.ir.Imm32(0);

        for (size_t j = 0; j < 4; j++) {
            const IR::U32 elem1 = (v.ir.*extension)(v.ir.VectorGetElement(8, operand1, 4 * i + j));
            const IR::U32 elem2 = (v.ir.*extension)(v.ir.VectorGetElement(8, operand2, 4 * index + j));

            res_element = v.ir.Add(res_element, v.ir.Mul(elem1, elem2));
        }

        res_element = v.ir.Add(v.ir.VectorGetElement(32, result, i), res_element);
        result = v.ir.VectorSetElement(32, result, i, res_element);
    }

    v.V(datasize, Vd, result);
    return true;
}

enum class Signedness {
    Signed,
    Unsigned
};

bool MultiplyLong(TranslatorVisitor& v, bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo,
                  Imm<1> H, Vec Vn, Vec Vd, ExtraBehavior extra_behavior, Signedness sign) {
    if (size == 0b00 || size == 0b11) {
        return v.UnallocatedEncoding();
    }

    const size_t idxsize = H == 1 ? 128 : 64;
    const size_t esize = 8 << size.ZeroExtend();
    const size_t datasize = 64;
    const auto [index, Vmhi] = [=] {
        if (size == 0b01) {
            return std::make_pair(concatenate(H, L, M).ZeroExtend(), Imm<1>{0});
        }

        return std::make_pair(concatenate(H, L).ZeroExtend(), M);
    }();

    const auto extend_operands = [&](const IR::U128& lhs, const IR::U128& rhs) {
        if (sign == Signedness::Signed) {
            return std::make_pair(v.ir.VectorSignExtend(esize, lhs),
                                  v.ir.VectorSignExtend(esize, rhs));
        }

        return std::make_pair(v.ir.VectorZeroExtend(esize, lhs),
                              v.ir.VectorZeroExtend(esize, rhs));
    };

    const IR::U128 operand1 = v.Vpart(datasize, Vn, Q);
    const IR::U128 operand2 = v.V(idxsize, concatenate(Vmhi, Vmlo).ZeroExtend<Vec>());
    const IR::U128 index_vector = v.ir.VectorBroadcast(esize, v.ir.VectorGetElement(esize, operand2, index));

    const IR::U128 result = [&] {
        const auto [extended_op1, extended_index] = extend_operands(operand1, index_vector);
        const IR::U128 product = v.ir.VectorMultiply(2 * esize, extended_op1, extended_index);
        if (extra_behavior == ExtraBehavior::None) {
            return product;
        }

        const IR::U128 operand3 = v.V(2 * datasize, Vd);
        if (extra_behavior == ExtraBehavior::Accumulate) {
            return v.ir.VectorAdd(2 * esize, operand3, product);
        }

        return v.ir.VectorSub(2 * esize, operand3, product);
    }();

    v.V(2 * datasize, Vd, result);
    return true;
}
} // Anonymous namespace

bool TranslatorVisitor::MLA_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyByElement(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Accumulate);
}

bool TranslatorVisitor::MLS_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyByElement(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Subtract);
}

bool TranslatorVisitor::MUL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyByElement(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::None);
}

bool TranslatorVisitor::FMLA_elt_4(bool Q, bool sz, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return FPMultiplyByElement(*this, Q, sz, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Accumulate);
}

bool TranslatorVisitor::FMLS_elt_4(bool Q, bool sz, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return FPMultiplyByElement(*this, Q, sz, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Subtract);
}

bool TranslatorVisitor::FMUL_elt_4(bool Q, bool sz, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return FPMultiplyByElement(*this, Q, sz, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::None);
}

bool TranslatorVisitor::SMLAL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyLong(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Accumulate, Signedness::Signed);
}

bool TranslatorVisitor::SMLSL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyLong(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Subtract, Signedness::Signed);
}

bool TranslatorVisitor::SMULL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyLong(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::None, Signedness::Signed);
}

bool TranslatorVisitor::SQDMULH_elt_2(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    if (size == 0b00 || size == 0b11) {
        return UnallocatedEncoding();
    }

    const size_t idxsize = H == 1 ? 128 : 64;
    const size_t esize = 8 << size.ZeroExtend();
    const size_t datasize = Q ? 128 : 64;
    const auto [index, Vmhi] = [=] {
        if (size == 0b01) {
            return std::make_pair(concatenate(H, L, M).ZeroExtend(), Imm<1>{0});
        }

        return std::make_pair(concatenate(H, L).ZeroExtend(), M);
    }();

    const IR::U128 operand1 = V(datasize, Vn);
    const IR::U128 operand2 = V(idxsize, concatenate(Vmhi, Vmlo).ZeroExtend<Vec>());
    const IR::U128 index_vector = ir.VectorBroadcast(esize, ir.VectorGetElement(esize, operand2, index));
    const IR::U128 result = ir.VectorSignedSaturatedDoublingMultiplyReturnHigh(esize, operand1, index_vector);

    V(datasize, Vd, result);
    return true;
}

bool TranslatorVisitor::SDOT_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return DotProduct(*this, Q, size, L, M, Vmlo, H, Vn, Vd, &IREmitter::SignExtendToWord);
}

bool TranslatorVisitor::UDOT_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return DotProduct(*this, Q, size, L, M, Vmlo, H, Vn, Vd, &IREmitter::ZeroExtendToWord);
}

bool TranslatorVisitor::UMLAL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyLong(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Accumulate, Signedness::Unsigned);
}

bool TranslatorVisitor::UMLSL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyLong(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::Subtract, Signedness::Unsigned);
}

bool TranslatorVisitor::UMULL_elt(bool Q, Imm<2> size, Imm<1> L, Imm<1> M, Imm<4> Vmlo, Imm<1> H, Vec Vn, Vec Vd) {
    return MultiplyLong(*this, Q, size, L, M, Vmlo, H, Vn, Vd, ExtraBehavior::None, Signedness::Unsigned);
}

} // namespace Dynarmic::A64
