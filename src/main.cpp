#include <vector>
#include <random>

#include <imgui/imgui.h>
#include <SDL2/SDL.h>

#include <emilib/gl_lib.hpp>
#include <emilib/gl_lib_opengl.hpp>
#include <emilib/gl_lib_sdl.hpp>
#include <emilib/imgui_gl_lib.hpp>
#include <emilib/imgui_helpers.hpp>
#include <emilib/imgui_sdl.hpp>
#include <emilib/irange.hpp>
#include <emilib/tga.hpp>
#include <loguru.hpp>

#include "sdf.hpp"

struct RGBA
{
	uint8_t r, g, b, a;
};

struct Circle
{
	bool   inverted   = false;
	size_t num_points = 64;

	float  center     =  0.5f;
	float  radius     =  0.35f;
};

struct Options
{
	int                 seed       =  0;
	// size_t                   resolution = 64;
	size_t              resolution = 13;
	std::vector<Circle> features;
	float               pos_noise  =  0.02f;
	float               dir_noise  =  0.05f;
	Strengths           strengths;

	Options()
	{
		features.push_back(Circle{});
		{
			Circle c;
			c.inverted = true;
			c.radius = 0.1;
			features.push_back(c);
		}
	}
};

struct Result
{
	std::vector<Point> points;
	std::vector<float> sdf;
	std::vector<RGBA>  sdf_image;
	std::vector<RGBA>  blob_image;
	float              blob_area;
};

void generate_points(std::vector<Point>* out_points, const Circle& options)
{
	CHECK_NOTNULL_F(out_points);
	float sign = options.inverted ? -1 : +1;

	for (size_t i = 0; i < options.num_points; ++i)
	{
		float angle = i * M_PI * 2 / options.num_points;
		float x = options.center + options.radius * std::cos(angle);
		float y = options.center + options.radius * std::sin(angle);
		float dx = sign * std::cos(angle);
		float dy = sign * std::sin(angle);
		out_points->push_back(Point{x, y, dx, dy});
	}
}

float area(const std::vector<Circle>& features)
{
	double expected_area = 0;
	for (const auto& circle : features) {
		float sign = circle.inverted ? -1 : +1;
		expected_area += sign * M_PI * std::pow(circle.radius, 2);
	}
	return expected_area;
}

Result generate(const Options& options)
{
	ERROR_CONTEXT("resolution", options.resolution);

	std::default_random_engine rng(options.seed);
	const int resolution = options.resolution;

	Result result;

	for (const auto& feature : options.features) {
		generate_points(&result.points, feature);
	}

	std::normal_distribution<float> pos_noise(0.0, options.pos_noise);
	std::normal_distribution<float> dir_noise(0.0, options.dir_noise);

	for (auto& point : result.points) {
		point.x += pos_noise(rng);
		point.y += pos_noise(rng);
		float angle = std::atan2(point.dy, point.dx);
		angle += dir_noise(rng);
		point.dx += std::cos(angle);
		point.dy += std::sin(angle);
	}

	result.sdf = generate_sdf(options.resolution, result.points, options.strengths);
	CHECK_EQ_F(result.sdf.size(), resolution * resolution);

	double area_pixels = 0;

	float max_abs_dist = 1;
	for (const float dist : result.sdf) {
		max_abs_dist = std::max(max_abs_dist, std::abs(dist));
	}

	for (const float dist : result.sdf) {
		const uint8_t dist_u8 = std::min<float>(255, 255 * std::abs(dist) / max_abs_dist);
		const uint8_t inv_dist_u8 = 255 - dist_u8;;
		if (dist < 0) {
			result.sdf_image.emplace_back(RGBA{inv_dist_u8, inv_dist_u8, 255, 255});
		} else {
			result.sdf_image.emplace_back(RGBA{255, inv_dist_u8, inv_dist_u8, 255});
		}

		float insideness = 1 - std::max(0.0, std::min(1.0, (dist + 0.5) * 2));
		const uint8_t color = 255 * insideness;
		result.blob_image.emplace_back(RGBA{color, color, color, 255});

		area_pixels += insideness;
	}

	result.blob_area = area_pixels / (resolution * resolution);

	return result;
}

bool showFeatureOption(Circle* circle)
{
	bool changed = false;

	ImGui::Text("Circle:");
	changed |= ImGui::Checkbox("inverted (hole)", &circle->inverted);
	changed |= ImGuiPP::SliderSize("num_points", &circle->num_points, 1, 1024, 2);
	changed |= ImGui::SliderFloat("center",      &circle->center,     0,    1);
	changed |= ImGui::SliderFloat("radius",      &circle->radius,     0,    1);

	return changed;
}

bool showStrengths(Strengths* strengths)
{
	bool changed = false;

	ImGui::Text("How much we trust the data:");
	changed |= ImGui::SliderFloat("data_pos",    &strengths->data_pos,    0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("data_normal", &strengths->data_normal, 0, 10, "%.4f", 4);
	ImGui::Text("How much we trust the model:");
	changed |= ImGui::SliderFloat("model_0",     &strengths->model_0,     0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("model_1",     &strengths->model_1,     0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("model_2",     &strengths->model_2,     0, 10, "%.4f", 4);

	return changed;
}

bool showOptions(Options* options)
{
	bool changed = false;

	if (ImGui::Button("Reset all")) {
		*options = {};
		changed = true;
	}
	changed |= ImGui::SliderInt("seed", &options->seed, 0, 100);
	changed |= ImGuiPP::SliderSize("resolution", &options->resolution, 4, 256);
	ImGui::Separator();
	for (const int i : emilib::indices(options->features)) {
		ImGui::PushID(i);
		changed |= showFeatureOption(&options->features[i]);
		ImGui::PopID();
	}
	ImGui::Separator();
	changed |= ImGui::SliderFloat("pos_noise", &options->pos_noise, 0, 1, "%.4f", 4);
	changed |= ImGui::SliderAngle("dir_noise", &options->dir_noise, 0,    360);
	ImGui::Separator();
	changed |= showStrengths(&options->strengths);

	return changed;
}

ImGuiWindowFlags fullscreen_window_flags()
{
	ImGuiIO& io = ImGui::GetIO();
	const float width = io.DisplaySize.x;
	const float height = io.DisplaySize.y;
	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiSetCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints({width, height}, {width, height});
	return ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
}

void draw_points(const Options& options, const std::vector<Point>& points, ImVec2 canvas_size)
{
	ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("canvas", canvas_size);
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	for (size_t i : emilib::irange<size_t>(0, options.resolution)) {
		const float left   = canvas_pos.x;
		const float right  = canvas_pos.x + canvas_size.x;
		const float top    = canvas_pos.y;
		const float bottom = canvas_pos.y + canvas_size.y;
		const float center_f = static_cast<float>(i) / (options.resolution - 1.0f);
		const float center_x = canvas_pos.x + canvas_size.x * center_f;
		const float center_y = canvas_pos.y + canvas_size.y * center_f;
		draw_list->AddLine({left, center_y}, {right, center_y}, ImColor(1.0f, 1.0f, 1.0f, 0.25f));
		draw_list->AddLine({center_x, top}, {center_x, bottom}, ImColor(1.0f, 1.0f, 1.0f, 0.25f));
	}

	for (const auto& point : points) {
		ImVec2 center;
		center.x = canvas_pos.x + canvas_size.x * point.x;
		center.y = canvas_pos.y + canvas_size.y * point.y;
		draw_list->AddCircleFilled(center, 1, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
		const float arrow_len = 5;
		draw_list->AddLine(center, ImVec2{center.x + arrow_len * point.dx, center.y + arrow_len * point.dy}, ImColor(1.0f, 1.0f, 1.0f, 0.75f));
	}
}

int main(int argc, char* argv[])
{
	loguru::g_colorlogtostderr = false;
	loguru::init(argc, argv);

	emilib::sdl::Params sdl_params;
	sdl_params.window_name = "2D SDF generator";
	sdl_params.width_points = 1800;
	sdl_params.height_points = 1200;
	auto sdl = emilib::sdl::init(sdl_params);

	emilib::ImGui_SDL imgui_sdl(sdl.width_points, sdl.height_points, sdl.pixels_per_point);

	gl::bind_imgui_painting();

	Options options;
	auto result = generate(options);

	gl::Texture sdf_texture{"sdf", gl::TexParams::clamped_nearest()};
	gl::Texture blob_texture{"blob", gl::TexParams::clamped_nearest()};

	const auto image_size = gl::Size{static_cast<unsigned>(options.resolution), static_cast<unsigned>(options.resolution)};
	sdf_texture.set_data(result.sdf_image.data(),   image_size, gl::ImageFormat::RGBA32);
	blob_texture.set_data(result.blob_image.data(), image_size, gl::ImageFormat::RGBA32);

	bool quit = false;
	while (!quit) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) { quit = true; }
			imgui_sdl.on_event(event);
		}
		imgui_sdl.new_frame();

		ImGui::ShowTestWindow();

		if (ImGui::Begin("Input")) {
			if (showOptions(&options)) {
				result = generate(options);
				const auto image_size = gl::Size{static_cast<unsigned>(options.resolution), static_cast<unsigned>(options.resolution)};
				sdf_texture.set_data(result.sdf_image.data(),   image_size, gl::ImageFormat::RGBA32);
				blob_texture.set_data(result.blob_image.data(), image_size, gl::ImageFormat::RGBA32);
			}
		}

		if (ImGui::Begin("Output")) {
			ImGui::Text("Model area: %.3f, sdf blob area: %.3f", area(options.features), result.blob_area);

			draw_points(options, result.points, {384, 384});

			ImGui::Image(reinterpret_cast<ImTextureID>(sdf_texture.id()), {384, 384});
			ImGui::SameLine();
			ImGui::Image(reinterpret_cast<ImTextureID>(blob_texture.id()), {384, 384});

			if (ImGui::Button("Save images")) {
				const auto res = options.resolution;
				const bool alpha = false;
				CHECK_F(emilib::write_tga("sdf.tga",  res, res, result.sdf_image.data(),  alpha));
				CHECK_F(emilib::write_tga("blob.tga", res, res, result.blob_image.data(), alpha));
			}
		}

		glClearColor(0.1f, 0.1f, 0.1f, 0);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		imgui_sdl.paint();

		SDL_GL_SwapWindow(sdl.window);
	}
}