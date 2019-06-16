// Copyright 2018 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mediapipe/framework/tool/status_util.h"

#include <memory>
#include <string>
#include <vector>

#include "mediapipe/framework/port/canonical_errors.h"
#include "mediapipe/framework/port/gmock.h"
#include "mediapipe/framework/port/gtest.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/framework/port/status_matchers.h"

namespace mediapipe {
namespace {

TEST(StatusTest, StatusStopIsNotOk) { EXPECT_FALSE(tool::StatusStop().ok()); }

TEST(StatusTest, Prefix) {
  const std::string base_error_message("error_with_this_string");
  const std::string prefix_error_message("error_with_prefix: ");
  ::mediapipe::Status base_status = ::mediapipe::Status(
      ::mediapipe::StatusCode::kInvalidArgument, base_error_message);
  ::mediapipe::Status status =
      tool::AddStatusPrefix(prefix_error_message, base_status);
  EXPECT_THAT(status.ToString(), testing::HasSubstr(base_error_message));
  EXPECT_THAT(status.ToString(), testing::HasSubstr(prefix_error_message));
  EXPECT_EQ(::mediapipe::StatusCode::kInvalidArgument, status.code());
}

TEST(StatusTest, CombinedStatus) {
  std::vector< ::mediapipe::Status> errors;
  const std::string prefix_error_message("error_with_prefix: ");
  ::mediapipe::Status status;

  errors.clear();
  errors.emplace_back(::mediapipe::StatusCode::kInvalidArgument,
                      "error_with_this_string");
  errors.emplace_back(::mediapipe::StatusCode::kInvalidArgument,
                      "error_with_that_string");
  status = tool::CombinedStatus(prefix_error_message, errors);
  EXPECT_THAT(status.ToString(), testing::HasSubstr(errors[0].error_message()));
  EXPECT_THAT(status.ToString(), testing::HasSubstr(errors[1].error_message()));
  EXPECT_THAT(status.ToString(), testing::HasSubstr(prefix_error_message));
  EXPECT_EQ(::mediapipe::StatusCode::kInvalidArgument, status.code());

  errors.clear();
  errors.emplace_back(::mediapipe::StatusCode::kNotFound,
                      "error_with_this_string");
  errors.emplace_back(::mediapipe::StatusCode::kInvalidArgument,
                      "error_with_that_string");
  status = tool::CombinedStatus(prefix_error_message, errors);
  EXPECT_THAT(status.ToString(), testing::HasSubstr(errors[0].error_message()));
  EXPECT_THAT(status.ToString(), testing::HasSubstr(errors[1].error_message()));
  EXPECT_THAT(status.ToString(), testing::HasSubstr(prefix_error_message));
  EXPECT_EQ(::mediapipe::StatusCode::kUnknown, status.code());
  errors.clear();
  errors.emplace_back(::mediapipe::StatusCode::kOk, "error_with_this_string");
  errors.emplace_back(::mediapipe::StatusCode::kInvalidArgument,
                      "error_with_that_string");
  status = tool::CombinedStatus(prefix_error_message, errors);
  EXPECT_THAT(status.ToString(), testing::HasSubstr(errors[1].error_message()));
  EXPECT_THAT(status.ToString(), testing::HasSubstr(prefix_error_message));
  EXPECT_EQ(::mediapipe::StatusCode::kInvalidArgument, status.code());

  errors.clear();
  errors.emplace_back(::mediapipe::StatusCode::kOk, "error_with_this_string");
  errors.emplace_back(::mediapipe::StatusCode::kOk, "error_with_that_string");
  MEDIAPIPE_EXPECT_OK(tool::CombinedStatus(prefix_error_message, errors));

  errors.clear();
  MEDIAPIPE_EXPECT_OK(tool::CombinedStatus(prefix_error_message, errors));
}

// Verify tool::StatusInvalid() and tool::StatusFail() and the alternatives
// recommended by their ABSL_DEPRECATED messages return the same
// ::mediapipe::Status objects.
TEST(StatusTest, Deprecated) {
  const std::string error_message = "an error message";
  EXPECT_EQ(tool::StatusInvalid(error_message),  // NOLINT
            ::mediapipe::InvalidArgumentError(error_message));
  EXPECT_EQ(tool::StatusFail(error_message),  // NOLINT
            ::mediapipe::UnknownError(error_message));
}

}  // namespace
}  // namespace mediapipe
