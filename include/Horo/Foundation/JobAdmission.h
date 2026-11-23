#pragma once

/** @file JobAdmission.h
 * @brief Typed classification of scheduler submission failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo {
    /** @brief Identifies capacity rejection from a submission on a lane that cannot wait.
     * @param error Error returned by a JobSystem submission.
     * @return True for a full or shedding queue, or a full blocking queue on a non-waiting lane.
     */
    [[nodiscard]] bool IsJobAdmissionPressure(const Error &error) noexcept;
}  // namespace Horo
