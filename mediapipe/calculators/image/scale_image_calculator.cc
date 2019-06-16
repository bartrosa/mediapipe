// Copyright 2019 The MediaPipe Authors.
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
//
// This Calculator takes an ImageFrame and scales it appropriately.

#include <algorithm>
#include <memory>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "libyuv/scale.h"
#include "mediapipe/calculators/image/scale_image_calculator.pb.h"
#include "mediapipe/calculators/image/scale_image_utils.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image_format.pb.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/formats/video_stream_header.h"
#include "mediapipe/framework/formats/yuv_image.h"
#include "mediapipe/framework/port/image_resizer.h"
#include "mediapipe/framework/port/integral_types.h"
#include "mediapipe/framework/port/logging.h"
#include "mediapipe/framework/port/opencv_core_inc.h"
#include "mediapipe/framework/port/proto_ns.h"
#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/util/image_frame_util.h"

namespace mediapipe {

namespace {

// Given an upscaling algorithm, determine which OpenCV interpolation algorithm
// to use.
::mediapipe::Status FindInterpolationAlgorithm(
    ScaleImageCalculatorOptions::ScaleAlgorithm upscaling_algorithm,
    int* interpolation_algorithm) {
  switch (upscaling_algorithm) {
    case ScaleImageCalculatorOptions::DEFAULT:
      *interpolation_algorithm = cv::INTER_CUBIC;
      break;
    case ScaleImageCalculatorOptions::LINEAR:
      *interpolation_algorithm = cv::INTER_LINEAR;
      break;
    case ScaleImageCalculatorOptions::CUBIC:
      *interpolation_algorithm = cv::INTER_CUBIC;
      break;
    case ScaleImageCalculatorOptions::AREA:
      *interpolation_algorithm = cv::INTER_AREA;
      break;
    case ScaleImageCalculatorOptions::LANCZOS:
      *interpolation_algorithm = cv::INTER_LANCZOS4;
      break;
    case ScaleImageCalculatorOptions::DEFAULT_WITHOUT_UPSCALE:
      *interpolation_algorithm = -1;
      break;
    default:
      RET_CHECK_FAIL() << absl::Substitute("Unknown upscaling algorithm: $0",
                                           upscaling_algorithm);
  }
  return ::mediapipe::OkStatus();
}

void CropImageFrame(const ImageFrame& original, int col_start, int row_start,
                    int crop_width, int crop_height, ImageFrame* cropped) {
  const uint8* src = original.PixelData();
  uint8* dst = cropped->MutablePixelData();

  int des_y = 0;
  for (int y = row_start; y < row_start + crop_height; ++y) {
    const uint8* src_line = src + y * original.WidthStep();
    const uint8* src_pixel = src_line + col_start *
                                            original.NumberOfChannels() *
                                            original.ByteDepth();
    uint8* dst_line = dst + des_y * cropped->WidthStep();
    std::memcpy(
        dst_line, src_pixel,
        crop_width * cropped->NumberOfChannels() * cropped->ByteDepth());
    ++des_y;
  }
}

}  // namespace

// Crops and scales an ImageFrame or YUVImage according to the options;
// The output can be cropped and scaled ImageFrame with the SRGB format. If the
// input is a YUVImage, the output can be a scaled YUVImage (the scaling is done
// using libyuv). Cropping is not yet supported for a YUVImage to a scaled
// YUVImage conversion.
//
// Example config:
// node {
//   calculator: "ScaleImageCalculator"
//   input_stream: "raw_frames"
//   output_stream: "scaled_frames"
//   node_options {
//     [type.googleapis.com/mediapipe.ScaleImageCalculatorOptions] {
//       target_width: 320
//       target_height: 320
//       preserve_aspect_ratio: true
//       output_format: SRGB
//       algorithm: DEFAULT
//     }
//   }
// }
//
// ScaleImageCalculator can also create or update a VideoHeader that is
// provided at Timestamp::PreStream on stream VIDEO_HEADER.
//
// Example config:
// node {
//   calculator: "ScaleImageCalculator"
//   input_stream: "FRAMES:ycbcr_frames"
//   input_stream: "VIDEO_HEADER:ycbcr_frames_header"  # Optional.
//   output_stream: "FRAMES:srgb_frames"
//   output_stream: "VIDEO_HEADER:srgb_frames_header"  # Independently Optional.
//   node_options {
//     [type.googleapis.com/mediapipe.ScaleImageCalculatorOptions] {
//       target_width: 320
//       target_height: 320
//       preserve_aspect_ratio: true
//       output_format: SRGB
//       algorithm: DEFAULT
//     }
//   }
// }
//
// The calculator options can be overrided with an input stream
// "OVERRIDE_OPTIONS". If this is provided, and non-empty at PreStream, the
// calculator options proto is merged with the proto provided in this packet
// (fields are overwritten in the original options) and the
// initialization happens in Process at PreStream, and not at Open.
class ScaleImageCalculator : public CalculatorBase {
 public:
  ScaleImageCalculator();
  ~ScaleImageCalculator() override;

  static ::mediapipe::Status GetContract(CalculatorContract* cc) {
    ScaleImageCalculatorOptions options =
        cc->Options<ScaleImageCalculatorOptions>();

    CollectionItemId input_data_id = cc->Inputs().GetId("FRAMES", 0);
    if (!input_data_id.IsValid()) {
      input_data_id = cc->Inputs().GetId("", 0);
    }
    CollectionItemId output_data_id = cc->Outputs().GetId("FRAMES", 0);
    if (!output_data_id.IsValid()) {
      output_data_id = cc->Outputs().GetId("", 0);
    }

    if (cc->Inputs().HasTag("VIDEO_HEADER")) {
      cc->Inputs().Tag("VIDEO_HEADER").Set<VideoHeader>();
    }
    if (options.has_input_format() &&
        options.input_format() == ImageFormat::YCBCR420P) {
      cc->Inputs().Get(input_data_id).Set<YUVImage>();
    } else {
      cc->Inputs().Get(input_data_id).Set<ImageFrame>();
    }

    if (cc->Outputs().HasTag("VIDEO_HEADER")) {
      cc->Outputs().Tag("VIDEO_HEADER").Set<VideoHeader>();
    }
    if (options.has_output_format() &&
        options.output_format() == ImageFormat::YCBCR420P) {
      RET_CHECK_EQ(ImageFormat::YCBCR420P, options.input_format());
      cc->Outputs().Get(output_data_id).Set<YUVImage>();
    } else {
      cc->Outputs().Get(output_data_id).Set<ImageFrame>();
    }

    if (cc->Inputs().HasTag("OVERRIDE_OPTIONS")) {
      cc->Inputs().Tag("OVERRIDE_OPTIONS").Set<ScaleImageCalculatorOptions>();
    }
    return ::mediapipe::OkStatus();
  }

  // From Calculator.
  ::mediapipe::Status Open(CalculatorContext* cc) override;
  ::mediapipe::Status Process(CalculatorContext* cc) override;

 private:
  // Initialize some data members from options_. This can be called either from
  // Open or Process depending on whether OVERRIDE_OPTIONS is used.
  ::mediapipe::Status InitializeFromOptions();
  // Initialize crop and output parameters based on set member variable
  // values.  This function will also send the header information on
  // the VIDEO_HEADER stream if it hasn't been done yet.
  ::mediapipe::Status InitializeFrameInfo(CalculatorContext* cc);
  // Validate that input_format_ and output_format_ are supported image
  // formats.
  ::mediapipe::Status ValidateImageFormats() const;
  // Validate that the image frame has the proper format and dimensions.
  // If the dimensions and format weren't initialized by the header,
  // then the first frame on which this function is called is used
  // to initialize.
  ::mediapipe::Status ValidateImageFrame(CalculatorContext* cc,
                                         const ImageFrame& image_frame);
  // Validate that the YUV image has the proper dimensions. If the
  // dimensions weren't initialized by the header, then the first image
  // on which this function is called is used to initialize.
  ::mediapipe::Status ValidateYUVImage(CalculatorContext* cc,
                                       const YUVImage& yuv_image);

  bool has_header_;  // True if the input stream has a header.
  int input_width_;
  int input_height_;
  int crop_width_;
  int crop_height_;
  int col_start_;
  int row_start_;
  int output_width_;
  int output_height_;
  ImageFormat::Format input_format_;
  ImageFormat::Format output_format_;
  int interpolation_algorithm_;

  // The "DATA" input stream.
  CollectionItemId input_data_id_;
  // The "DATA" output stream.
  CollectionItemId output_data_id_;
  VideoHeader input_video_header_;

  // Whether the header information was sent on the VIDEO_HEADER stream.
  bool header_sent_ = false;

  // The alignment boundary that newly created images should have.
  int alignment_boundary_;

  ScaleImageCalculatorOptions options_;

  // Efficient image resizer with gamma correction and optional sharpening.
  std::unique_ptr<ImageResizer> downscaler_;
};

REGISTER_CALCULATOR(ScaleImageCalculator);

ScaleImageCalculator::ScaleImageCalculator() {}

ScaleImageCalculator::~ScaleImageCalculator() {}

::mediapipe::Status ScaleImageCalculator::InitializeFrameInfo(
    CalculatorContext* cc) {
  RETURN_IF_ERROR(
      scale_image::FindCropDimensions(input_width_, input_height_,  //
                                      options_.min_aspect_ratio(),  //
                                      options_.max_aspect_ratio(),  //
                                      &crop_width_, &crop_height_,  //
                                      &col_start_, &row_start_));
  RETURN_IF_ERROR(
      scale_image::FindOutputDimensions(crop_width_, crop_height_,            //
                                        options_.target_width(),              //
                                        options_.target_height(),             //
                                        options_.preserve_aspect_ratio(),     //
                                        options_.scale_to_multiple_of_two(),  //
                                        &output_width_, &output_height_));
  RETURN_IF_ERROR(FindInterpolationAlgorithm(options_.algorithm(),
                                             &interpolation_algorithm_));
  if (interpolation_algorithm_ == -1 &&
      (output_width_ > crop_width_ || output_height_ > crop_height_)) {
    output_width_ = crop_width_;
    output_height_ = crop_height_;
  }
  VLOG(1) << "Image scaling parameters:"
          << "\ninput_width_ " << input_width_      //
          << "\ninput_height_ " << input_height_    //
          << "\ninput_format_ " << input_format_    //
          << "\ncrop_width_ " << crop_width_        //
          << "\ncrop_height_ " << crop_height_      //
          << "\ncol_start_ " << col_start_          //
          << "\nrow_start_ " << row_start_          //
          << "\noutput_width_ " << output_width_    //
          << "\noutput_height_ " << output_height_  //
          << "\noutput_format_ " << output_format_  //
          << "\nOpenCV interpolation algorithm " << interpolation_algorithm_;
  if (!header_sent_ && cc->Outputs().UsesTags() &&
      cc->Outputs().HasTag("VIDEO_HEADER")) {
    header_sent_ = true;
    auto header = absl::make_unique<VideoHeader>();
    *header = input_video_header_;
    header->width = output_width_;
    header->height = output_height_;
    header->format = output_format_;
    LOG(INFO) << "OUTPUTTING HEADER on stream";
    cc->Outputs()
        .Tag("VIDEO_HEADER")
        .Add(header.release(), Timestamp::PreStream());
    cc->Outputs().Tag("VIDEO_HEADER").Close();
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ScaleImageCalculator::Open(CalculatorContext* cc) {
  options_ = cc->Options<ScaleImageCalculatorOptions>();

  input_data_id_ = cc->Inputs().GetId("FRAMES", 0);
  if (!input_data_id_.IsValid()) {
    input_data_id_ = cc->Inputs().GetId("", 0);
  }
  output_data_id_ = cc->Outputs().GetId("FRAMES", 0);
  if (!output_data_id_.IsValid()) {
    output_data_id_ = cc->Outputs().GetId("", 0);
  }

  // The output packets are at the same timestamp as the input.
  cc->Outputs().Get(output_data_id_).SetOffset(mediapipe::TimestampDiff(0));

  has_header_ = false;
  input_width_ = 0;
  input_height_ = 0;
  crop_width_ = 0;
  crop_height_ = 0;
  output_width_ = 0;
  output_height_ = 0;
  bool has_override_options = cc->Inputs().HasTag("OVERRIDE_OPTIONS");

  if (!has_override_options) {
    RETURN_IF_ERROR(InitializeFromOptions());
  }

  if (!cc->Inputs().Get(input_data_id_).Header().IsEmpty()) {
    // If the input stream has a header then our output stream also has a
    // header.

    if (has_override_options) {
      // It's not possible to use OVERRIDE_OPTIONS when the main input stream
      // has a header. At this point in the code, the ScaleImageCalculator
      // config may be changed by the new options at PreStream, so the output
      // header can't be determined.
      return ::mediapipe::InvalidArgumentError(
          "OVERRIDE_OPTIONS stream can't be used when the main input stream "
          "has a header.");
    }
    input_video_header_ =
        cc->Inputs().Get(input_data_id_).Header().Get<VideoHeader>();

    input_format_ = input_video_header_.format;
    if (options_.has_input_format()) {
      RET_CHECK_EQ(input_format_, options_.input_format())
          << "The input header format does not match the input_format option.";
    }

    input_width_ = input_video_header_.width;
    input_height_ = input_video_header_.height;

    if (options_.has_output_format()) {
      output_format_ = options_.output_format();
    } else {
      output_format_ = input_format_;
    }

    if (output_format_ == ImageFormat::YCBCR420P) {
      RET_CHECK(options_.scale_to_multiple_of_two())
          << "ScaleImageCalculator always outputs width and height that are "
             "divisible by 2 when output format is YCbCr420P. To scale to "
             "width and height of odd numbers, the output format must be SRGB.";
    } else if (options_.preserve_aspect_ratio()) {
      RET_CHECK(options_.scale_to_multiple_of_two())
          << "ScaleImageCalculator always outputs width and height that are "
             "divisible by 2 when perserving aspect ratio. To scale to width "
             "and height of odd numbers, please set "
             "preserve_aspect_ratio to false.";
    }

    if (input_width_ > 0 && input_height_ > 0 &&
        input_format_ != ImageFormat::UNKNOWN &&
        output_format_ != ImageFormat::UNKNOWN) {
      RETURN_IF_ERROR(ValidateImageFormats());
      RETURN_IF_ERROR(InitializeFrameInfo(cc));
      std::unique_ptr<VideoHeader> output_header(new VideoHeader());
      *output_header = input_video_header_;
      output_header->format = output_format_;
      output_header->width = output_width_;
      output_header->height = output_height_;
      cc->Outputs()
          .Get(output_data_id_)
          .SetHeader(Adopt(output_header.release()));
      has_header_ = true;
    } else {
      LOG(WARNING) << "Stream had a VideoHeader which didn't have sufficient "
                      "information.  "
                      "Dropping VideoHeader and trying to deduce needed "
                      "information.";
      input_width_ = 0;
      input_height_ = 0;
      if (!options_.has_input_format()) {
        input_format_ = ImageFormat::UNKNOWN;
      }
      output_format_ = ImageFormat::UNKNOWN;
    }
  }

  return ::mediapipe::OkStatus();
}

::mediapipe::Status ScaleImageCalculator::InitializeFromOptions() {
  if (options_.has_input_format()) {
    input_format_ = options_.input_format();
  } else {
    input_format_ = ImageFormat::UNKNOWN;
  }

  alignment_boundary_ = 16;
  if (options_.alignment_boundary() > 0) {
    alignment_boundary_ = options_.alignment_boundary();
  }

  downscaler_.reset(new ImageResizer(options_.post_sharpening_coefficient()));

  return ::mediapipe::OkStatus();
}

::mediapipe::Status ScaleImageCalculator::ValidateImageFormats() const {
  RET_CHECK_NE(input_format_, ImageFormat::UNKNOWN)
      << "The input image format was UNKNOWN.";
  RET_CHECK_NE(output_format_, ImageFormat::UNKNOWN)
      << "The output image format was set to UNKNOWN.";
  // TODO Remove these conditions.
  RET_CHECK(output_format_ == ImageFormat::SRGB ||
            (input_format_ == output_format_ &&
             output_format_ == ImageFormat::YCBCR420P))
      << "Outputting YCbCr420P images from SRGB input is not yet supported";
  RET_CHECK(input_format_ == output_format_ ||
            input_format_ == ImageFormat::YCBCR420P)
      << "Conversion of the color space (except from "
         "YCbCr420P to SRGB) is not yet supported.";
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ScaleImageCalculator::ValidateImageFrame(
    CalculatorContext* cc, const ImageFrame& image_frame) {
  if (!has_header_) {
    if (input_width_ != image_frame.Width() ||
        input_height_ != image_frame.Height() ||
        input_format_ != image_frame.Format()) {
      // Set the dimensions based on the image frame.  There was no header.
      input_width_ = image_frame.Width();
      input_height_ = image_frame.Height();
      RET_CHECK(input_width_ > 0 && input_height_ > 0) << absl::StrCat(
          "The input image did not have positive dimensions. dimensions: ",
          input_width_, "x", input_height_);
      input_format_ = image_frame.Format();
      if (options_.has_input_format()) {
        RET_CHECK_EQ(input_format_, options_.input_format())
            << "The input image format does not match the input_format option.";
      }
      if (options_.has_output_format()) {
        output_format_ = options_.output_format();
      } else {
        output_format_ = input_format_;
      }
      RETURN_IF_ERROR(InitializeFrameInfo(cc));
    }
    RETURN_IF_ERROR(ValidateImageFormats());
  } else {
    if (input_width_ != image_frame.Width() ||
        input_height_ != image_frame.Height()) {
      return tool::StatusFail(absl::StrCat(
          "If a header specifies a width and a height, then image frames on "
          "the stream must have that size.  Received frame of size ",
          image_frame.Width(), "x", image_frame.Height(), " but expected ",
          input_width_, "x", input_height_));
    }
    if (input_format_ != image_frame.Format()) {
      const proto_ns::EnumDescriptor* desc = ImageFormat::Format_descriptor();
      return tool::StatusFail(absl::StrCat(
          "If a header specifies a format, then image frames on "
          "the stream must have that format.  Actual format ",
          desc->FindValueByNumber(image_frame.Format())->DebugString(),
          " but expected ",
          desc->FindValueByNumber(input_format_)->DebugString()));
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ScaleImageCalculator::ValidateYUVImage(
    CalculatorContext* cc, const YUVImage& yuv_image) {
  CHECK_EQ(input_format_, ImageFormat::YCBCR420P);
  if (!has_header_) {
    if (input_width_ != yuv_image.width() ||
        input_height_ != yuv_image.height()) {
      // Set the dimensions based on the YUV image.  There was no header.
      input_width_ = yuv_image.width();
      input_height_ = yuv_image.height();
      RET_CHECK(input_width_ > 0 && input_height_ > 0) << absl::StrCat(
          "The input image did not have positive dimensions. dimensions: ",
          input_width_, "x", input_height_);
      if (options_.has_output_format()) {
        output_format_ = options_.output_format();
      } else {
        output_format_ = input_format_;
      }
      RETURN_IF_ERROR(InitializeFrameInfo(cc));
    }
    RETURN_IF_ERROR(ValidateImageFormats());
  } else {
    if (input_width_ != yuv_image.width() ||
        input_height_ != yuv_image.height()) {
      return tool::StatusFail(absl::StrCat(
          "If a header specifies a width and a height, then YUV images on "
          "the stream must have that size.  Additionally, all YUV images in "
          "a stream must have the same size.  Received frame of size ",
          yuv_image.width(), "x", yuv_image.height(), " but expected ",
          input_width_, "x", input_height_));
    }
  }
  return ::mediapipe::OkStatus();
}

::mediapipe::Status ScaleImageCalculator::Process(CalculatorContext* cc) {
  if (cc->InputTimestamp() == Timestamp::PreStream()) {
    if (cc->Inputs().HasTag("OVERRIDE_OPTIONS")) {
      if (cc->Inputs().Tag("OVERRIDE_OPTIONS").IsEmpty()) {
        return ::mediapipe::InvalidArgumentError(
            "The OVERRIDE_OPTIONS input stream must be non-empty at PreStream "
            "time if used.");
      }
      options_.MergeFrom(cc->Inputs()
                             .Tag("OVERRIDE_OPTIONS")
                             .Get<ScaleImageCalculatorOptions>());
      RETURN_IF_ERROR(InitializeFromOptions());
    }
    if (cc->Inputs().UsesTags() && cc->Inputs().HasTag("VIDEO_HEADER") &&
        !cc->Inputs().Tag("VIDEO_HEADER").IsEmpty()) {
      input_video_header_ = cc->Inputs().Tag("VIDEO_HEADER").Get<VideoHeader>();
    }
    if (cc->Inputs().Get(input_data_id_).IsEmpty()) {
      return ::mediapipe::OkStatus();
    }
  }

  cc->GetCounter("Inputs")->Increment();
  const ImageFrame* image_frame;
  ImageFrame converted_image_frame;
  if (input_format_ == ImageFormat::YCBCR420P) {
    const YUVImage* yuv_image =
        &cc->Inputs().Get(input_data_id_).Get<YUVImage>();
    RETURN_IF_ERROR(ValidateYUVImage(cc, *yuv_image));

    if (output_format_ == ImageFormat::SRGB) {
      // TODO: For ease of implementation, YUVImage is converted to
      // ImageFrame immediately, before cropping and scaling. Investigate how to
      // make color space conversion more efficient when cropping or scaling is
      // also needed.
      image_frame_util::YUVImageToImageFrame(*yuv_image, &converted_image_frame,
                                             options_.use_bt709());
      image_frame = &converted_image_frame;
    } else if (output_format_ == ImageFormat::YCBCR420P) {
      RET_CHECK(row_start_ == 0 && col_start_ == 0 &&
                crop_width_ == input_width_ && crop_height_ == input_height_)
          << "ScaleImageCalculator only supports scaling on YUVImages. To crop "
             "images, the output format must be SRGB.";

      // Scale the YUVImage and output without converting the color space.
      const int y_size = output_width_ * output_height_;
      const int uv_size = output_width_ * output_height_ / 4;
      std::unique_ptr<uint8_t[]> yuv_data(new uint8_t[y_size + uv_size * 2]);
      uint8* y = yuv_data.get();
      uint8* u = y + y_size;
      uint8* v = u + uv_size;
      RET_CHECK_EQ(0, I420Scale(yuv_image->data(0), yuv_image->stride(0),
                                yuv_image->data(1), yuv_image->stride(1),
                                yuv_image->data(2), yuv_image->stride(2),
                                yuv_image->width(), yuv_image->height(), y,
                                output_width_, u, output_width_ / 2, v,
                                output_width_ / 2, output_width_,
                                output_height_, libyuv::kFilterBox));
      auto output_image = absl::make_unique<YUVImage>(
          libyuv::FOURCC_I420, std::move(yuv_data), y, output_width_, u,
          output_width_ / 2, v, output_width_ / 2, output_width_,
          output_height_);
      cc->GetCounter("Outputs Scaled")->Increment();
      if (yuv_image->width() >= output_width_ &&
          yuv_image->height() >= output_height_) {
        cc->GetCounter("Downscales")->Increment();
      } else if (interpolation_algorithm_ != -1) {
        cc->GetCounter("Upscales")->Increment();
      }
      cc->Outputs()
          .Get(output_data_id_)
          .Add(output_image.release(), cc->InputTimestamp());
      return ::mediapipe::OkStatus();
    }
  } else {
    image_frame = &cc->Inputs().Get(input_data_id_).Get<ImageFrame>();
    RETURN_IF_ERROR(ValidateImageFrame(cc, *image_frame));
  }

  std::unique_ptr<ImageFrame> cropped_image;
  if (crop_width_ < input_width_ || crop_height_ < input_height_) {
    cc->GetCounter("Crops")->Increment();
    // TODO Do the crop as a range restrict inside OpenCV code below.
    cropped_image.reset(new ImageFrame(image_frame->Format(), crop_width_,
                                       crop_height_, alignment_boundary_));
    if (image_frame->ByteDepth() == 1 || image_frame->ByteDepth() == 2) {
      CropImageFrame(*image_frame, col_start_, row_start_, crop_width_,
                     crop_height_, cropped_image.get());
    } else {
      return tool::StatusInvalid(
          "Input format does not have ByteDepth of 1 or 2.");
    }

    // Update the image_frame to point to the cropped image.  The
    // unique_ptr will take care of deleting the cropped image when the
    // function returns.
    image_frame = cropped_image.get();
  }

  // Skip later operations if no scaling is necessary.
  if (crop_width_ == output_width_ && crop_height_ == output_height_) {
    // Efficiently use either the cropped image or the original image.
    if (image_frame == cropped_image.get()) {
      if (options_.set_alignment_padding()) {
        cropped_image->SetAlignmentPaddingAreas();
      }
      cc->GetCounter("Outputs Cropped")->Increment();
      cc->Outputs()
          .Get(output_data_id_)
          .Add(cropped_image.release(), cc->InputTimestamp());
    } else {
      if (options_.alignment_boundary() <= 0 &&
          (!options_.set_alignment_padding() || image_frame->IsContiguous())) {
        // Any alignment is acceptable and we don't need to clear the
        // alignment padding (either because the user didn't request it
        // or because the data is contiguous).
        cc->GetCounter("Outputs Inputs")->Increment();
        cc->Outputs()
            .Get(output_data_id_)
            .AddPacket(cc->Inputs().Get(input_data_id_).Value());
      } else {
        // Make a copy with the correct alignment.
        std::unique_ptr<ImageFrame> output_frame(new ImageFrame());
        output_frame->CopyFrom(*image_frame, alignment_boundary_);
        if (options_.set_alignment_padding()) {
          output_frame->SetAlignmentPaddingAreas();
        }
        cc->GetCounter("Outputs Aligned")->Increment();
        cc->Outputs()
            .Get(output_data_id_)
            .Add(output_frame.release(), cc->InputTimestamp());
      }
    }
    return ::mediapipe::OkStatus();
  }

  // Rescale the image frame.
  std::unique_ptr<ImageFrame> output_frame(new ImageFrame());
  if (image_frame->Width() >= output_width_ &&
      image_frame->Height() >= output_height_) {
    // Downscale.
    cc->GetCounter("Downscales")->Increment();
    cv::Mat input_mat = ::mediapipe::formats::MatView(image_frame);
    output_frame->Reset(image_frame->Format(), output_width_, output_height_,
                        alignment_boundary_);
    cv::Mat output_mat = ::mediapipe::formats::MatView(output_frame.get());
    downscaler_->Resize(input_mat, &output_mat);
  } else {
    // Upscale. If upscaling is disallowed, output_width_ and output_height_ are
    // the same as the input/crop width and height.
    image_frame_util::RescaleImageFrame(
        *image_frame, output_width_, output_height_, alignment_boundary_,
        interpolation_algorithm_, output_frame.get());
    if (interpolation_algorithm_ != -1) {
      cc->GetCounter("Upscales")->Increment();
    }
  }

  if (options_.set_alignment_padding()) {
    cc->GetCounter("Pads")->Increment();
    output_frame->SetAlignmentPaddingAreas();
  }

  cc->GetCounter("Outputs Scaled")->Increment();
  cc->Outputs()
      .Get(output_data_id_)
      .Add(output_frame.release(), cc->InputTimestamp());
  return ::mediapipe::OkStatus();
}

}  // namespace mediapipe
