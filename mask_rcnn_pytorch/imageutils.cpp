#include "imageutils.h"
#include "debug.h"
#include "nnutils.h"

cv::Mat LoadImage(const std::string path) {
  cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
  return image;
}

at::Tensor CvImageToTensor(const cv::Mat& image) {
  // Idea taken from https://github.com/pytorch/pytorch/issues/12506
  // we have to split the interleaved channels
  assert(image.channels() == 3);
  cv::Mat bgr[3];
  cv::split(image, bgr);
  cv::Mat channelsConcatenated;
  cv::vconcat(bgr[2], bgr[1], channelsConcatenated);
  cv::vconcat(channelsConcatenated, bgr[0], channelsConcatenated);

  cv::Mat channelsConcatenatedFloat;
  channelsConcatenated.convertTo(channelsConcatenatedFloat, CV_32FC3);
  assert(channelsConcatenatedFloat.isContinuous());

  std::vector<int64_t> dims{static_cast<int64_t>(image.channels()),
                            static_cast<int64_t>(image.rows),
                            static_cast<int64_t>(image.cols)};

  at::TensorOptions options(at::kFloat);
  at::Tensor tensor_image =
      torch::from_blob(channelsConcatenated.data, at::IntList(dims),
                       options.requires_grad(false))
          .clone();  // clone is required to copy data from temporary object
  return tensor_image;
}

std::tuple<cv::Mat, Window, float, Padding> ResizeImage(cv::Mat image,
                                                        int32_t min_dim,
                                                        int32_t max_dim,
                                                        bool do_padding) {
  // Default window (y1, x1, y2, x2) and default scale == 1.
  auto h = image.rows;
  auto w = image.cols;
  Window window{0, 0, h, w};
  Padding padding;
  float scale = 1.f;

  // Scale?
  if (min_dim != 0) {
    // Scale up but not down
    scale = std::max(1.f, static_cast<float>(min_dim) / std::min(h, w));
  }

  // Does it exceed max dim?
  if (max_dim != 0) {
    auto image_max = std::max(h, w);
    if (std::round(image_max * scale) > max_dim)
      scale = static_cast<float>(max_dim) / image_max;
  }
  // Resize image and mask
  if (scale != 1.f) {
    cv::resize(image, image,
               cv::Size(static_cast<int>(std::round(w * scale)),
                        static_cast<int>(std::round(h * scale))),
               cv::INTER_LINEAR);
  }
  // Need padding?
  if (do_padding) {
    // Get new height and width
    h = image.rows;
    w = image.cols;
    auto top_pad = (max_dim - h) / 2;
    auto bottom_pad = max_dim - h - top_pad;
    auto left_pad = (max_dim - w) / 2;
    auto right_pad = max_dim - w - left_pad;
    cv::copyMakeBorder(image, image, top_pad, bottom_pad, left_pad, right_pad,
                       cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    padding = {top_pad, bottom_pad, left_pad, right_pad, 0, 0};
    window = {top_pad, left_pad, h + top_pad, w + left_pad};
  }
  return {image, window, scale, padding};
}

/*
 * Takes RGB images with 0-255 values and subtraces
 * the mean pixel and converts it to float. Expects image
 * colors in RGB order.
 */
cv::Mat MoldImage(cv::Mat image, const Config& config) {
  assert(image.channels() == 3);
  cv::Scalar mean(config.mean_pixel[2], config.mean_pixel[1],
                  config.mean_pixel[0]);
  image -= mean;
  return image;
}

std::tuple<at::Tensor, std::vector<ImageMeta>, std::vector<Window>> MoldInputs(
    const std::vector<cv::Mat>& images,
    const Config& config) {
  std::vector<at::Tensor> molded_images;
  std::vector<ImageMeta> image_metas;
  std::vector<Window> windows;
  for (const auto& image : images) {
    // Resize image to fit the model expected size
    auto [molded_image, window, scale, padding] =
        ResizeImage(image, config.image_min_dim, config.image_max_dim,
                    config.image_padding);
    molded_image.convertTo(molded_image, CV_32FC3);
    molded_image = MoldImage(molded_image, config);

    // Build image_meta
    ImageMeta image_meta{0, image.rows, image.cols, window,
                         std::vector<int32_t>(config.num_classes, 0)};

    // To tensor
    auto img_t = CvImageToTensor(molded_image);

    // Append
    molded_images.push_back(img_t);
    windows.push_back(window);
    image_metas.push_back(image_meta);
  }
  // Pack into arrays
  auto tensor_images = torch::stack(molded_images);
  // To GPU
  if (config.gpu_count > 0)
    tensor_images = tensor_images.cuda();

  return {tensor_images, image_metas, windows};
}

/*
 * Converts a mask generated by the neural network into a format similar
 * to it's original shape.
 * mask: [height, width] of type float. A small, typically 28x28 mask.
 * bbox: [y1, x1, y2, x2]. The box to fit the mask in.
 * Returns a binary mask with the same size as the original image.
 */
cv::Mat UnmoldMask(at::Tensor mask,
                   at::Tensor bbox,
                   const cv::Size& image_shape) {
  const double threshold = 0.5;
  auto y1 = *bbox[0].data<int32_t>();
  auto x1 = *bbox[1].data<int32_t>();
  auto y2 = *bbox[2].data<int32_t>();
  auto x2 = *bbox[3].data<int32_t>();

  cv::Mat cv_mask(static_cast<int>(mask.size(0)),
                  static_cast<int>(mask.size(1)), CV_32FC1, mask.data<float>());
  cv::resize(cv_mask, cv_mask, cv::Size(x2 - x1, y2 - y1));
  cv::threshold(cv_mask, cv_mask, threshold, 1, cv::THRESH_BINARY);
  cv_mask *= 255;

  cv::Mat full_mask = cv::Mat::zeros(image_shape, CV_32FC1);
  cv_mask.copyTo(full_mask(cv::Rect(x1, y1, x2 - x1, y2 - y1)));

  full_mask.convertTo(full_mask, CV_8UC1);
  return full_mask;
}

std::tuple<at::Tensor, at::Tensor, at::Tensor, std::vector<cv::Mat>>
UnmoldDetections(at::Tensor detections,
                 at::Tensor mrcnn_mask,
                 const cv::Size& image_shape,
                 const Window& window) {
  // How many detections do we have?
  // Detections array is padded with zeros. Find the first class_id == 0.
  auto zero_ix = (detections.narrow(1, 4, 1) == 0).nonzero();
  if (zero_ix.size(0) > 0)
    zero_ix = zero_ix[0];
  auto N =
      zero_ix.size(0) > 0 ? *zero_ix[0].data<uint8_t>() : detections.size(0);

  //  Extract boxes, class_ids, scores, and class-specific masks
  auto boxes = detections.narrow(0, 0, N).narrow(1, 0, 4);  //[:N, :4];
  auto class_ids = detections.narrow(0, 0, N)
                       .narrow(1, 4, 1)
                       .to(at::dtype(at::kLong))
                       .squeeze();
  auto scores = detections.narrow(0, 0, N).narrow(1, 5, 1);
  auto masks = index_select_2d(torch::arange(N, at::dtype(at::kLong)),
                               class_ids, mrcnn_mask,
                               3);  // [np.arange(N), :, :, class_ids];

  // Compute scale and shift to translate coordinates to image domain.
  auto h_scale =
      static_cast<float>(image_shape.height) / (window.y2 - window.y1);
  auto w_scale =
      static_cast<float>(image_shape.width) / (window.x2 - window.x1);
  auto scale = std::min(h_scale, w_scale);
  auto scales = torch::tensor({scale, scale, scale, scale});
  auto shifts = torch::tensor(
      {static_cast<float>(window.y1), static_cast<float>(window.x1),
       static_cast<float>(window.y1), static_cast<float>(window.x1)});

  // Translate bounding boxes to image domain
  boxes = ((boxes - shifts) * scales).to(at::dtype(at::kInt));

  // Filter out detections with zero area. Often only happens in early
  // stages of training when the network weights are still a bit random.
  auto include_ix = (((boxes.narrow(1, 2, 1) - boxes.narrow(1, 0, 1)) *
                      (boxes.narrow(1, 3, 1) - boxes.narrow(1, 1, 1))) > 0)
                        .nonzero();
  include_ix = include_ix.narrow(1, 0, 1).squeeze();
  if (include_ix.size(0) > 0) {
    boxes = boxes.index_select(0, include_ix);
    class_ids = class_ids.index_select(0, include_ix);
    scores = scores.index_select(0, include_ix);
    masks = masks.index_select(0, include_ix);
    N = class_ids.size(0);
  } else {
    // TODO: make  empty tensors
  }
  // Resize masks to original image size and set boundary threshold.
  std::vector<cv::Mat> full_masks_vec;
  for (int64_t i = 0; i < N; ++i) {
    // Convert neural network mask to full size mask
    auto full_mask = UnmoldMask(masks[i], boxes[i], image_shape);
    full_masks_vec.push_back(full_mask);
  }

  return {boxes, class_ids, scores, full_masks_vec};
}

std::vector<cv::Mat> ResizeMasks(std::vector<cv::Mat> masks,
                                 float scale,
                                 const Padding& padding) {
  std::vector<cv::Mat> res_masks;
  for (const auto& mask : masks) {
    cv::Mat m;
    cv::resize(mask, m,
               cv::Size(static_cast<int>(std::round(mask.cols * scale)),
                        static_cast<int>(std::round(mask.rows * scale))),
               cv::INTER_LINEAR);

    cv::copyMakeBorder(m, m, padding.top_pad, padding.bottom_pad,
                       padding.left_pad, padding.right_pad, cv::BORDER_CONSTANT,
                       cv::Scalar(0, 0, 0));

    res_masks.push_back(m);
  }
  return res_masks;
}
