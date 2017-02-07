#include <random>
#include <sstream>
#include <vector>

extern "C" {
	#include <heman.h>
}

#include <SDL2/SDL.h>
#include <stb/stb_image.h>

#include <emilib/dual.hpp>
#include <emilib/file_system.hpp>
#include <emilib/gl_lib.hpp>
#include <emilib/gl_lib_opengl.hpp>
#include <emilib/gl_lib_sdl.hpp>
#include <emilib/imgui_gl_lib.hpp>
#include <emilib/imgui_helpers.hpp>
#include <emilib/imgui_sdl.hpp>
#include <emilib/irange.hpp>
#include <emilib/marching_squares.hpp>
#include <emilib/math.hpp>
#include <emilib/scope_exit.hpp>
#include <emilib/tga.hpp>
#include <emilib/timer.hpp>
#include <loguru.hpp>

#include <field_interpolation/field_interpolation.hpp>
#include <field_interpolation/sparse_linear.hpp>

#include "serialize_configuru.hpp"

namespace fi = field_interpolation;

VISITABLE_STRUCT(ImVec2, x, y);
VISITABLE_STRUCT(fi::Weights, data_pos, data_gradient, model_0, model_1, model_2, model_3, model_4, gradient_smoothness);
VISITABLE_STRUCT(fi::SolveOptions, downscale_factor, tile, tile_size, cg, error_tolerance);

using Vec2List = std::vector<ImVec2>;

struct RGBA
{
	uint8_t r, g, b, a;
};

struct Shape
{
	bool   inverted        = false;
	size_t num_points      = 64;
	float  lopsidedness[2] = {1.0f, 1.0f};

	ImVec2 center          = {0.5f, 0.5f};
	float  radius          =  0.35f;

	float  circleness      =  0;
	size_t polygon_sides   =  3;

	float  rotation        =  0;
};

VISITABLE_STRUCT(Shape, inverted, num_points, lopsidedness, center, radius, circleness, polygon_sides, rotation);

struct NoiseOptions
{
	int    seed          = 0;
	float  pos_stddev    = 0.005f;
	float  normal_stddev = 0.05f;
	size_t outliers      = 0;
};

VISITABLE_STRUCT(NoiseOptions, seed, pos_stddev, normal_stddev, outliers);

struct Options
{
	NoiseOptions       noise;
	size_t             resolution  = 24;
	std::vector<Shape> shapes;
	fi::Weights        weights;
	bool               exact_solve = false;
	fi::SolveOptions   solve_options;

	Options()
	{
		shapes.push_back(Shape{});
		{
			Shape c;
			c.inverted = true;
			c.radius = 0.1;
			c.circleness = 1;
			shapes.push_back(c);
		}
	}
};

VISITABLE_STRUCT(Options, noise, resolution, shapes, weights, exact_solve, solve_options);

struct Result
{
	Vec2List           point_positions;
	Vec2List           point_normals;
	fi::LatticeField   field;
	std::vector<float> sdf;
	std::vector<float> heatmap;
	std::vector<RGBA>  sdf_image;
	std::vector<RGBA>  blob_image;
	std::vector<RGBA>  heatmap_image;
	float              blob_area;
	double             duration_seconds;
};

using Dualf = emilib::Dual<float>;

std::vector<RGBA> generate_heatmap(const std::vector<float>& data, float min, float max)
{
	int colormap_width, colormap_height;
	RGBA* colormap = reinterpret_cast<RGBA*>(stbi_load("colormap_jet.png", &colormap_width, &colormap_height, nullptr, 4));
	CHECK_NOTNULL_F(colormap, "Failed to load colormap: %s", stbi_failure_reason());
	SCOPE_EXIT{ stbi_image_free(colormap); };

	std::vector<RGBA> colors;
	for (float value : data) {
		if (max <= min) {
			colors.emplace_back(RGBA{0,0,0,255});
		} else {
			int colormap_x = math::remap_clamp(value, min, max, 0, colormap_width - 1);
			colors.emplace_back(colormap[colormap_x]);
		}
	}
	return colors;
}

auto circle_point(const Shape& shape, Dualf t) -> std::pair<Dualf, Dualf>
{
	Dualf angle = t * math::TAUf + shape.rotation;
	return std::make_pair(std::cos(angle), std::sin(angle));
}

auto poly_point(const Shape& shape, Dualf t) -> std::pair<Dualf, Dualf>
{
	CHECK_GE_F(shape.polygon_sides, 3u);

	auto polygon_corner = [&](int corner) {
		float angle = math::TAUf * corner / shape.polygon_sides;
		angle += shape.rotation;
		return ImVec2(std::cos(angle), std::sin(angle));
	};

	int corner_0 = t.real * shape.polygon_sides;

	auto v0 = polygon_corner(corner_0);
	auto v1 = polygon_corner(corner_0 + 1);

	Dualf side_t = t * float(shape.polygon_sides) - float(corner_0);

	Dualf x = v0.x + side_t * (v1.x - v0.x);
	Dualf y = v0.y + side_t * (v1.y - v0.y);
	return std::make_pair(x, y);
}

/// t = [0, 1] along perimeter
auto shape_point(const Shape& shape, Dualf t)
{
	Dualf circle_x, circle_y;
	Dualf poly_x,   poly_y;
	std::tie(circle_x, circle_y) = circle_point(shape, t);
	std::tie(poly_x,   poly_y)   = poly_point(shape,   t);

	Dualf x = math::lerp(poly_x, circle_x, shape.circleness);
	Dualf y = math::lerp(poly_y, circle_y, shape.circleness);

	return std::make_pair(ImVec2(x.real, y.real), ImVec2(y.eps, -x.eps));
}

void generate_points(
	Vec2List*    out_positions,
	Vec2List*    out_normals,
	const Shape& shape,
	size_t       min_points)
{
	CHECK_NOTNULL_F(out_positions);
	size_t num_points = std::max(shape.num_points, min_points);

	auto add_point_at = [&](float t) {
		ImVec2 pos, normal;
		auto td = shape.inverted ? Dualf(1.0f - t, -1.0f) : Dualf(t, 1.0f);
		std::tie(pos, normal) = shape_point(shape, td);

		pos.x = shape.center.x + shape.radius * pos.x;
		pos.y = shape.center.y + shape.radius * pos.y;

		out_positions->emplace_back(pos);
		if (out_normals) {
			out_normals->emplace_back(normal);
		}
	};

	int num_points_in_first_half = std::round(shape.lopsidedness[0] * num_points / 2);
	for (size_t i = 0; i < num_points_in_first_half; ++i) {
		add_point_at(0.5f * float(i) / num_points_in_first_half);
	}

	int num_points_in_second_half = std::round(shape.lopsidedness[1] * num_points / 2);
	for (size_t i = 0; i < num_points_in_second_half; ++i) {
		add_point_at(0.5f + 0.5f * i / num_points_in_second_half);
	}
}

float area(const std::vector<Shape>& shapes)
{
	double expected_area = 0;
	for (const auto& shape : shapes) {
		Vec2List positions;
		generate_points(&positions, nullptr, shape, 2048);

		std::vector<float> line_segments;
		for (const auto i : emilib::indices(positions)) {
			line_segments.push_back(positions[i].x);
			line_segments.push_back(positions[i].y);
			line_segments.push_back(positions[(i + 1) % positions.size()].x);
			line_segments.push_back(positions[(i + 1) % positions.size()].y);
		}
		expected_area += emilib::calc_area(line_segments.size() / 4, line_segments.data());
	}
	return expected_area;
}

auto generate_sdf(const Vec2List& positions, const Vec2List& normals, const Options& options)
{
	LOG_SCOPE_F(1, "generate_sdf");
	CHECK_EQ_F(positions.size(), normals.size());

	const int width = options.resolution;
	const int height = options.resolution;

	static_assert(sizeof(ImVec2) == 2 * sizeof(float), "Pack");
	const auto field = sdf_from_points(
		{width, height}, options.weights, positions.size(), &positions[0].x, &normals[0].x, nullptr);

	const size_t num_unknowns = width * height;
	std::vector<float> sdf;
	if (options.exact_solve) {
		sdf = solve_sparse_linear(num_unknowns, field.eq.triplets, field.eq.rhs);
	} else {
		sdf = solve_sparse_linear_approximate_lattice(
			field.eq.triplets, field.eq.rhs, {width, height}, options.solve_options);
	}
	if (sdf.size() != num_unknowns) {
		LOG_F(ERROR, "Failed to find a solution");
		sdf.resize(num_unknowns, 0.0f);
	}

	return std::make_tuple(field, sdf);
}

void perturb_points(Vec2List* positions, Vec2List* normals, const NoiseOptions& options)
{
	std::default_random_engine rng(options.seed);
	std::normal_distribution<float> pos_noise(0.0, options.pos_stddev);
	std::normal_distribution<float> dir_noise(0.0, options.normal_stddev);

	for (auto& pos : *positions) {
		pos.x += pos_noise(rng);
		pos.y += pos_noise(rng);
	}
	for (auto& normal : *normals) {
		float angle = std::atan2(normal.y, normal.x);
		angle += dir_noise(rng);
		normal.x = std::cos(angle);
		normal.y = std::sin(angle);
	}

	std::uniform_real_distribution<float> random_pos(0.0f, 1.0f);
	std::normal_distribution<float> random_normal;
	for (size_t i = 0; i < options.outliers; ++i) {
		positions->emplace_back(random_pos(rng), random_pos(rng));
		normals->emplace_back(random_normal(rng), random_normal(rng));
	}

	for (auto& normal : *normals) {
		normal /= std::hypot(normal.x, normal.y);
	}
}

Result generate(const Options& options)
{
	ERROR_CONTEXT("resolution", options.resolution);

	emilib::Timer timer;
	const int resolution = options.resolution;

	Result result;

	for (const auto& shape : options.shapes) {
		generate_points(&result.point_positions, &result.point_normals, shape, 0);
	}
	perturb_points(&result.point_positions, &result.point_normals, options.noise);

	Vec2List lattice_positions;

	for (const auto& pos : result.point_positions) {
		ImVec2 on_lattice = pos;
		on_lattice.x *= (resolution - 1.0f);
		on_lattice.y *= (resolution - 1.0f);
		lattice_positions.push_back(on_lattice);
	}

	std::tie(result.field, result.sdf) = generate_sdf(lattice_positions, result.point_normals, options);
	result.heatmap = generate_error_map(result.field.eq.triplets, result.sdf, result.field.eq.rhs);
	result.heatmap_image = generate_heatmap(result.heatmap, 0, *max_element(result.heatmap.begin(), result.heatmap.end()));
	CHECK_EQ_F(result.heatmap_image.size(), resolution * resolution);

	double area_pixels = 0;

	float max_abs_dist = 1e-6f;
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

	result.blob_area = area_pixels / math::sqr(resolution - 1);

	result.duration_seconds = timer.secs();
	return result;
}

bool show_shape_options(Shape* shape)
{
	bool changed = false;

	ImGui::Text("Shape:");
	changed |= ImGui::Checkbox("inverted (hole)",   &shape->inverted);
	changed |= ImGuiPP::SliderSize("num_points",    &shape->num_points,    1, 100000, 4);
	changed |= ImGui::SliderFloat2("lopsidedness",  shape->lopsidedness,   0,    2);
	changed |= ImGui::SliderFloat2("center",        &shape->center.x,      0,    1);
	changed |= ImGui::SliderFloat("radius",         &shape->radius,        0,    1);
	changed |= ImGui::SliderFloat("circleness",     &shape->circleness,   -1,    5);
	changed |= ImGuiPP::SliderSize("polygon_sides", &shape->polygon_sides, 3,    8);
	changed |= ImGui::SliderAngle("rotation",       &shape->rotation,      0,  360);
	return changed;
}

bool show_weights(fi::Weights* weights)
{
	bool changed = false;

	ImGui::Text("Gradient kernel:");
	ImGui::SameLine();
	changed |= ImGuiPP::RadioButtonEnum("nearest-neighbor", &weights->gradient_kernel, fi::GradientKernel::kNearestNeighbor);
	ImGui::SameLine();
	changed |= ImGuiPP::RadioButtonEnum("cell edges", &weights->gradient_kernel, fi::GradientKernel::kCellEdges);
	ImGui::SameLine();
	changed |= ImGuiPP::RadioButtonEnum("n-linear-interpolation", &weights->gradient_kernel, fi::GradientKernel::kLinearInteprolation);

	if (ImGui::Button("Reset weights")) {
		*weights = {};
		changed = true;
	}
	ImGui::Text("How much we trust the data:");
	changed |= ImGui::SliderFloat("data_pos",      &weights->data_pos,      0, 1000, "%.3f", 4);
	changed |= ImGui::SliderFloat("data_gradient", &weights->data_gradient, 0, 1000, "%.3f", 4);
	ImGui::Text("How much we trust the model:");
	changed |= ImGui::SliderFloat("f(0) = 0 (regularization)",    &weights->model_0, 0, 1000, "%.3f", 4);
	changed |= ImGui::SliderFloat("f'(0) = 0 (flatness)",         &weights->model_1, 0, 1000, "%.3f", 4);
	changed |= ImGui::SliderFloat("f''(0) = 0 (C1 smoothness)",   &weights->model_2, 0, 1000, "%.3f", 4);
	changed |= ImGui::SliderFloat("f'''(0) = 0 (C2 smoothness)",  &weights->model_3, 0, 1000, "%.3f", 4);
	changed |= ImGui::SliderFloat("f''''(0) = 0 (C3 smoothness)", &weights->model_4, 0, 1000, "%.3f", 4);
	changed |= ImGui::SliderFloat("Gradient smoothness)", &weights->gradient_smoothness, 0, 1000, "%.3f", 4);

	return changed;
}

bool show_solve_options(fi::SolveOptions* options)
{
	bool changed = false;
	if (ImGui::Button("Reset solve options")) {
		*options = {};
		changed = true;
	}
	changed |= ImGui::SliderInt("downscale_factor", &options->downscale_factor, 2, 10);
	changed |= ImGui::Checkbox("tile", &options->tile);
	if (options->tile) {
		changed |= ImGui::SliderInt("tile_size", &options->tile_size, 2, 128);
	}
	changed |= ImGui::Checkbox("cg", &options->cg);
	if (options->cg) {
		changed |= ImGui::SliderFloat("error_tolerance", &options->error_tolerance, 1e-6f, 1, "%.6f", 4);
	}
	return changed;
}

bool show_noise_options(NoiseOptions* options)
{
	bool changed = false;
	ImGui::PushItemWidth(ImGui::GetWindowContentRegionWidth() * 0.3f);
	ImGui::Text("Noise:");
	changed |= ImGui::SliderInt("seed      ", &options->seed, 0, 100);
	ImGui::SameLine();
	changed |= ImGuiPP::SliderSize("outliers", &options->outliers, 0, 50);
	changed |= ImGui::SliderFloat("pos_stddev", &options->pos_stddev, 0,   0.1, "%.4f");
	ImGui::SameLine();
	changed |= ImGui::SliderAngle("normal_stddev", &options->normal_stddev, 0, 360);
	ImGui::PopItemWidth();
	return changed;
}

bool show_options(Options* options)
{
	bool changed = false;

	if (ImGui::Button("Reset all")) {
		*options = {};
		changed = true;
	}
	changed |= ImGuiPP::SliderSize("resolution", &options->resolution, 4, 256);
	ImGui::Separator();
	for (const int i : emilib::indices(options->shapes)) {
		ImGui::PushID(i);
		changed |= show_shape_options(&options->shapes[i]);
		ImGui::PopID();
		ImGui::Separator();
	}
	if (options->shapes.size() >= 2 && ImGui::Button("Remove shape")) {
		options->shapes.pop_back();
		changed = true;
		ImGui::SameLine();
	}
	if (ImGui::Button("Add shape")) {
		options->shapes.push_back(Shape{});
		changed = true;
	}
	ImGui::Separator();
	changed |= show_noise_options(&options->noise);
	ImGui::Separator();
	changed |= show_weights(&options->weights);

	changed |= ImGui::Checkbox("Exact solve", &options->exact_solve);
	if (!options->exact_solve) {
		changed |= show_solve_options(&options->solve_options);
	}

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

void show_texture_options(gl::Texture* texture)
{
	auto params = texture->params();
	ImGui::Text("Filter:");
	ImGui::SameLine();
	int is_nearest = params.filter == gl::TexFilter::Nearest;
	ImGui::RadioButton("Nearest", &is_nearest, 1);
	ImGui::SameLine();
	ImGui::RadioButton("Linear", &is_nearest, 0);
	params.filter = is_nearest ? gl::TexFilter::Nearest : gl::TexFilter::Linear;
	texture->set_params(params);
}

void show_cells(const Options& options, ImVec2 canvas_pos, ImVec2 canvas_size)
{
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	if (options.resolution < 64) {
		// Draw sample points
		for (size_t xi : emilib::irange<size_t>(0, options.resolution)) {
			for (size_t yi : emilib::irange<size_t>(0, options.resolution)) {
				const float x = static_cast<float>(xi) / (options.resolution - 1.0f);
				const float y = static_cast<float>(yi) / (options.resolution - 1.0f);
				const float center_x = canvas_pos.x + canvas_size.x * x;
				const float center_y = canvas_pos.y + canvas_size.y * y;
				draw_list->AddCircleFilled({center_x, center_y}, 1, ImColor(1.0f, 1.0f, 1.0f, 0.25f), 4);
			}
		}
	}
}

void show_points(const Options& options, const Vec2List& positions, const Vec2List& normals,
	ImVec2 canvas_pos, ImVec2 canvas_size)
{
	CHECK_EQ_F(positions.size(), normals.size());
	if (positions.size() > 2000) { return; }

	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	for (const auto pi : emilib::indices(positions)) {
		ImVec2 center;
		center.x = canvas_pos.x + canvas_size.x * positions[pi].x;
		center.y = canvas_pos.y + canvas_size.y * positions[pi].y;
		draw_list->AddCircleFilled(center, 1, ImColor(1.0f, 1.0f, 1.0f, 1.0f), 4);

		if (positions.size() < 1000) {
			const float arrow_len = 5;
			draw_list->AddLine(center, ImVec2{center.x + arrow_len * normals[pi].x, center.y + arrow_len * normals[pi].y}, ImColor(1.0f, 1.0f, 1.0f, 0.75f));
		}
	}
}

void show_outline(
	size_t                    resolution,
	const std::vector<float>& lines,
	ImVec2                    canvas_pos,
	ImVec2                    canvas_size,
	ImColor                   color,
	bool                      draw_blob_normals)
{
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	CHECK_F(lines.size() % 4 == 0);

	for (size_t i = 0; i < lines.size(); i += 4) {
		float x0 = lines[i + 0];
		float y0 = lines[i + 1];
		float x1 = lines[i + 2];
		float y1 = lines[i + 3];

		x0 = canvas_pos.x + canvas_size.x * (x0 / (resolution - 1.0f));
		y0 = canvas_pos.y + canvas_size.y * (y0 / (resolution - 1.0f));
		x1 = canvas_pos.x + canvas_size.x * (x1 / (resolution - 1.0f));
		y1 = canvas_pos.y + canvas_size.y * (y1 / (resolution - 1.0f));

		draw_list->AddLine({x0, y0}, {x1, y1}, color);

		if (draw_blob_normals) {
			float cx = (x0 + x1) / 2;
			float cy = (y0 + y1) / 2;
			float dx = (x1 - x0);
			float dy = (y1 - y0);
			float norm = 10 / std::hypot(dx, dy);
			dx *= norm;
			dy *= norm;
			draw_list->AddLine({cx, cy}, {cx + dy, cy - dx}, ImColor(0.0f, 1.0f, 0.0f, 1.0f));
		}
	}
}

void show_field_equations(const fi::LatticeField& field)
{
	std::stringstream ss;
	ss << field.eq;
	std::string eq_str = ss.str();
	ImGui::Text("%lu equations:\n", field.eq.rhs.size());
	ImGui::TextUnformatted(eq_str.c_str());
}

// ----------------------------------------------------------------------------

struct Point1D
{
	float pos, value, gradient;
};
VISITABLE_STRUCT(Point1D, pos, value, gradient);

struct Field1DInput
{
	std::vector<Point1D> points{
		{0.2f, 0, +1},
		{0.8f, 0, -1},
	};

	int resolution = 12;
	fi::Weights weights;
};
VISITABLE_STRUCT(Field1DInput, points, resolution, weights);

Field1DInput load_1d_field()
{
	Field1DInput input;
	if (fs::file_exists("1d_field.json")) {
		const auto config = configuru::parse_file("1d_field.json", configuru::JSON);
		from_config(&input, config);
	}
	return input;
}

bool show_options(Field1DInput* input)
{
	bool changed = false;

	changed |= ImGui::SliderInt("resolution", &input->resolution, 4, 512);
	changed |= show_weights(&input->weights);

	for (const auto i : emilib::indices(input->points)) {
		auto& point = input->points[i];
		ImGui::PushID(i);
		ImGui::PushItemWidth(ImGui::GetWindowContentRegionWidth() * 0.25f);
		ImGui::Text("Point %lu:", i);
		ImGui::SameLine();
		changed |= ImGui::SliderFloat("pos", &point.pos, 0, 1);
		ImGui::SameLine();
		changed |= ImGui::SliderFloat("value", &point.value, 0, 1);
		ImGui::SameLine();
		changed |= ImGui::SliderFloat("gradient", &point.gradient, -1, 1);
		ImGui::PopItemWidth();
		ImGui::PopID();
	}

	if (input->points.size() >= 2 && ImGui::Button("Remove point")) {
		input->points.pop_back();
		changed = true;
		ImGui::SameLine();
	}
	if (ImGui::Button("Add point")) {
		auto point = input->points.back();
		input->points.push_back(point);
		changed = true;
	}

	return changed;
}

void show_1d_field_window(Field1DInput* input)
{
	if (show_options(input)) {
		configuru::dump_file("1d_field.json", to_config(*input), configuru::JSON);
	}

	fi::LatticeField field{{input->resolution}};

	for (const auto& point : input->points) {
		float pos_lattice = point.pos * (input->resolution - 1);
		float gradient_lattice = point.gradient / (input->resolution - 1);
		add_value_constraint(&field, &pos_lattice, point.value, input->weights.data_pos);
		add_gradient_constraint(&field, &pos_lattice, &gradient_lattice, input->weights.data_gradient, input->weights.gradient_kernel);
	}

	add_field_constraints(&field, input->weights);

	const size_t num_unknowns = input->resolution;
	auto interpolated = solve_sparse_linear(num_unknowns, field.eq.triplets, field.eq.rhs);
	if (interpolated.size() != num_unknowns) {
		LOG_F(ERROR, "Failed to find a solution");
		interpolated.resize(num_unknowns, 0.0f);
	}
	ImGui::Text("interpolated: %f %f ...", interpolated[0], interpolated[1]);

	ImVec2 canvas_size{384, 384};
	ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("canvas", canvas_size);
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	auto canvas_from_field = [=](float x, float y) {
		return ImVec2{
			canvas_pos.x + canvas_size.x * x,
			canvas_pos.y + canvas_size.y * (1 - y)};
	};

	for (size_t i : emilib::irange_inclusive<size_t>(0, 1)) {
		draw_list->AddLine(canvas_from_field(0, i), canvas_from_field(1, i), ImColor(1.0f, 1.0f, 1.0f, 0.25f));
		draw_list->AddLine(canvas_from_field(i, 0), canvas_from_field(i, 1), ImColor(1.0f, 1.0f, 1.0f, 0.25f));
	}

	std::vector<ImVec2> field_points;
	for (size_t i : emilib::indices(interpolated)) {
		field_points.push_back(canvas_from_field(i / (input->resolution - 1.0f), interpolated[i]));
		draw_list->AddCircleFilled(field_points.back(), 2, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
	}

	draw_list->AddPolyline(field_points.data(), field_points.size(), ImColor(1.0f, 1.0f, 1.0f, 1.0f), false, 2, true);

	for (const auto& point : input->points) {
		float arrow_len = 16;
		ImVec2 point_pos = canvas_from_field(point.pos, point.value);
		draw_list->AddCircleFilled(point_pos, 5, ImColor(1.0f, 0.0f, 0.0f, 1.0f));
		if (input->weights.data_gradient > 0) {
			ImVec2 gradient_offset = {arrow_len, -point.gradient * arrow_len};
			draw_list->AddLine(point_pos - gradient_offset, point_pos + gradient_offset, ImColor(1.0f, 0.0f, 0.0f, 0.5f), 2);
		}
	}

	show_field_equations(field);
}

// ----------------------------------------------------------------------------

void show_1d_denoiser_window()
{
	static int         s_seed           =   0;
	static int         s_resolution     = 512;
	static float       s_noise_y_stddev =   0.1f;
	static size_t      s_num_points     = 256;
	static float       s_amplitude      =   0.5f;
	static float       s_freq           =   10.0f;
	static float       s_chirp_factor   =   2.0f;
	static fi::Weights s_weights = [](){
		fi::Weights weights;
		weights.model_1 = 0;
		weights.model_2 = 10;
		return weights;
	}();

	// ------------------------------------------

	std::default_random_engine rng(s_seed);
	std::normal_distribution<float> y_noise(0.0, s_noise_y_stddev);

	Vec2List points;
	Vec2List gt;

	for (int i : emilib::irange(s_num_points)) {
		float t = math::remap(i, 0, s_num_points - 1, 0, 1);
		float f = s_freq * (1 + t * s_chirp_factor);
		float y = s_amplitude * std::sin(t * f);
		gt.emplace_back(t, y);
		y += y_noise(rng);
		points.emplace_back(t, y);
	}

	// ------------------------------------------

	fi::LatticeField field{{s_resolution}};

	add_field_constraints(&field, s_weights);

	for (const ImVec2& p : points) {
		float x = math::remap(p.x, 0, 1, 0, s_resolution - 1.0f);
		add_value_constraint(&field, &x, p.y, s_weights.data_pos);
	}

	const int num_unknowns = s_resolution;
	std::vector<float> solution = solve_sparse_linear(num_unknowns, field.eq.triplets, field.eq.rhs);
	if (solution.empty()) { solution.resize(num_unknowns, 0.0f); }

	// ------------------------------------------

	ImGui::SliderInt("resolution",       &s_resolution,     10,  1000);
	ImGui::SliderFloat("noise",          &s_noise_y_stddev,  0,     1);
	ImGuiPP::SliderSize("points",        &s_num_points,      0, 10000);
	ImGui::SliderFloat("s_amplitude",    &s_amplitude,       0,     1);
	ImGui::SliderFloat("s_freq",         &s_freq,            0,     100);
	ImGui::SliderFloat("s_chirp_factor", &s_chirp_factor,    0,    10);
	show_weights(&s_weights);

	ImVec2 canvas_size = ImGui::GetContentRegionAvail();
	ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("canvas", canvas_size);
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	auto canvas_from_field = [=](float t, float y) {
		return ImVec2{
			canvas_pos.x + canvas_size.x * t,
			canvas_pos.y + math::remap(y, -1, +1, canvas_size.y, 0)
		};
	};

	draw_list->AddLine(canvas_pos, canvas_pos + ImVec2(canvas_size.x, 0), ImColor(1.0f, 1.0f, 1.0f, 0.25f));
	draw_list->AddLine(canvas_pos, canvas_pos + ImVec2(0, canvas_size.y), ImColor(1.0f, 1.0f, 1.0f, 0.25f));
	draw_list->AddLine(canvas_pos + canvas_size, canvas_pos + ImVec2(canvas_size.x, 0), ImColor(1.0f, 1.0f, 1.0f, 0.25f));
	draw_list->AddLine(canvas_pos + canvas_size, canvas_pos + ImVec2(0, canvas_size.y), ImColor(1.0f, 1.0f, 1.0f, 0.25f));

	for (const ImVec2& p : points) {
		draw_list->AddCircleFilled(canvas_from_field(p.x, p.y), 2, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
	}

	Vec2List gt_points;
	for (const ImVec2& point : gt) {
		gt_points.push_back(canvas_from_field(point.x, point.y));
	}

	draw_list->AddPolyline(gt_points.data(), gt_points.size(), ImColor(1.0f, 0.0f, 0.0f, 0.5f), false, 2, true);

	Vec2List solution_points;
	for (auto i : emilib::irange(num_unknowns)) {
		float t = math::remap(i, 0, num_unknowns - 1, 0, 1);
		solution_points.push_back(canvas_from_field(t, solution[i]));
	}

	draw_list->AddPolyline(solution_points.data(), solution_points.size(), ImColor(1.0f, 1.0f, 1.0f, 1.0f), false, 2, true);
}

// ----------------------------------------------------------------------------

void show_2d_field_window()
{
	// Based on https://en.wikipedia.org/wiki/Multivariate_interpolation
	float values[4 * 4] = {
		5, 4, 2, 3,
		4, 2, 1, 5,
		6, 3, 5, 2,
		1, 2, 4, 1,
	};

	static int         s_resolution = 64;
	static fi::Weights     s_weights;
	static gl::Texture s_texture{"2d_field", gl::TexParams::clamped_nearest()};

	ImGui::SliderInt("resolution", &s_resolution, 4, 64);
	show_weights(&s_weights);

	fi::LatticeField field{{s_resolution, s_resolution}};
	add_field_constraints(&field, s_weights);

	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			const float pos[2] = {
				math::remap(x, 0.0f, 3.0f, 0.0f, s_resolution - 1.0f),
				math::remap(y, 0.0f, 3.0f, 0.0f, s_resolution - 1.0f),
			};
			add_value_constraint(&field, pos, values[y * 4 + x], s_weights.data_pos);
			const float zero[2] = {0,0};
			add_gradient_constraint(&field, pos, zero, s_weights.data_gradient, s_weights.gradient_kernel);
		}
	}

	const size_t num_unknowns = s_resolution * s_resolution;
	auto interpolated = solve_sparse_linear(num_unknowns, field.eq.triplets, field.eq.rhs);
	if (interpolated.size() != num_unknowns) {
		LOG_F(ERROR, "Failed to find a solution");
		interpolated.resize(num_unknowns, 0.0f);
	}

	const auto heatmap = generate_heatmap(interpolated, 0, 6);
	const auto image_size = gl::Size{static_cast<unsigned>(s_resolution), static_cast<unsigned>(s_resolution)};
	s_texture.set_data(heatmap.data(), image_size, gl::ImageFormat::RGBA32);

	ImVec2 canvas_size{384, 384};
	show_texture_options(&s_texture);
	ImGui::Image(reinterpret_cast<ImTextureID>(s_texture.id()), canvas_size);

	// show_field_equations(field);
}

std::vector<float> iso_surface(int width, int height, const float* values, float iso)
{
	std::vector<float> iso_at_zero;
	iso_at_zero.reserve(width * height);
	for (int i = 0; i < width * height; ++i) {
		iso_at_zero.push_back(values[i] - iso);
	}

	return emilib::marching_squares(width, height, iso_at_zero.data());
}

struct FieldGui
{
	Options options;
	Result result;
	gl::Texture sdf_texture{"sdf", gl::TexParams::clamped_nearest()};
	gl::Texture blob_texture{"blob", gl::TexParams::clamped_nearest()};
	gl::Texture heatmap_texture{"heatmap", gl::TexParams::clamped_nearest()};
	bool draw_points = true;
	bool draw_cells = true;
	bool draw_iso_lines = true;
	bool draw_blob_normals = false;
	float iso_spacing = 2;

	FieldGui()
	{
		if (fs::file_exists("sdf_input.json")) {
			const auto config = configuru::parse_file("sdf_input.json", configuru::JSON);
			from_config(&options, config);
		}
		calc();
	}

	void calc()
	{
		result = generate(options);
		const auto image_size = gl::Size{static_cast<unsigned>(options.resolution), static_cast<unsigned>(options.resolution)};
		sdf_texture.set_data(result.sdf_image.data(),         image_size, gl::ImageFormat::RGBA32);
		blob_texture.set_data(result.blob_image.data(),       image_size, gl::ImageFormat::RGBA32);
		heatmap_texture.set_data(result.heatmap_image.data(), image_size, gl::ImageFormat::RGBA32);
	}

	void show_input()
	{
		if (show_options(&options)) {
			calc();
			const auto config = to_config(options);
			configuru::dump_file("sdf_input.json", config, configuru::JSON);
		}
	}

	void show_result()
	{
		const float iso_min = *min_element(result.sdf.begin(), result.sdf.end());
		const float iso_max = *max_element(result.sdf.begin(), result.sdf.end());

		std::vector<float> zero_lines = iso_surface(options.resolution, options.resolution, result.sdf.data(), 0.0f);
		const float lines_area = emilib::calc_area(zero_lines.size() / 4, zero_lines.data()) / math::sqr(options.resolution - 1);

		ImGui::Text("%lu unknowns", options.resolution * options.resolution);
		ImGui::Text("%lu equations", result.field.eq.rhs.size());
		ImGui::Text("%lu non-zero values in matrix", result.field.eq.triplets.size());
		ImGui::Text("Calculated in %.3f s", result.duration_seconds);
		ImGui::Text("Model area: %.3f, marching squares area: %.3f, sdf blob area: %.3f",
			area(options.shapes), lines_area, result.blob_area);

		ImGui::Checkbox("Input points", &draw_points);
		ImGui::SameLine();
		ImGui::Checkbox("Input cells", &draw_cells);
		ImGui::SameLine();
		ImGui::Checkbox("Output blob", &draw_iso_lines);
		if (draw_iso_lines) {
			ImGui::SameLine();
			ImGui::Checkbox("Output normals", &draw_blob_normals);
			ImGui::SameLine();
			ImGui::PushItemWidth(128);
			ImGui::SliderFloat("Iso spacing", &iso_spacing, 1, 10, "%.0f");
		}

		ImVec2 available = ImGui::GetContentRegionAvail();
		float image_width = std::floor(std::min(available.x / 2, (available.y - 64) / 2));
		ImVec2 canvas_size{image_width, image_width};
		ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("canvas", canvas_size);
		if (draw_cells) { show_cells(options, canvas_pos, canvas_size); }
		if (draw_points) { show_points(options, result.point_positions, result.point_normals, canvas_pos, canvas_size); }
		if (draw_iso_lines) {
			for (int i = math::floor_to_int(iso_min / iso_spacing); i <= math::ceil_to_int(iso_max / iso_spacing); ++i) {
				auto iso_lines = iso_surface(options.resolution, options.resolution, result.sdf.data(), i * iso_spacing);
				int color = i == 0 ? ImColor(1.0f, 0.0f, 0.0f, 1.0f) : ImColor(0.5f, 0.5f, 0.5f, 0.5f);
				show_outline(options.resolution, iso_lines, canvas_pos, canvas_size, color, i == 0 && draw_blob_normals);
			}
		}

		blob_texture.set_params(sdf_texture.params());
		heatmap_texture.set_params(sdf_texture.params());

		// HACK to apply the params:
		sdf_texture.bind(); blob_texture.bind(); heatmap_texture.bind();

		ImGui::SameLine();
		ImGui::Image(reinterpret_cast<ImTextureID>(heatmap_texture.id()), canvas_size);

		ImGui::Text("Max error: %f", *max_element(result.heatmap.begin(), result.heatmap.end()));

		ImGui::Image(reinterpret_cast<ImTextureID>(sdf_texture.id()), canvas_size);
		ImGui::SameLine();
		ImGui::Image(reinterpret_cast<ImTextureID>(blob_texture.id()), canvas_size);

		ImGui::Text("Field min: %f, max: %f", iso_min, iso_max);

		show_texture_options(&sdf_texture);
		ImGui::SameLine();
		if (ImGui::Button("Save images")) {
			const auto res = options.resolution;
			const bool alpha = false;
			CHECK_F(emilib::write_tga("heatmap.tga", res, res, result.heatmap_image.data(), alpha));
			CHECK_F(emilib::write_tga("sdf.tga",     res, res, result.sdf_image.data(),     alpha));
			CHECK_F(emilib::write_tga("blob.tga",    res, res, result.blob_image.data(),    alpha));
		}
	}
};

void show_sdf_fields(FieldGui* field_gui)
{
	if (ImGui::Begin("2D SDF")) {
		ImGui::BeginChild("Input", ImVec2(ImGui::GetWindowContentRegionWidth() * 0.35f, 0), true);
		field_gui->show_input();
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("Output", ImVec2(ImGui::GetWindowContentRegionWidth() * 0.65f, 0), true);
		field_gui->show_result();
		ImGui::EndChild();
	}
	ImGui::End();
}

// ----------------------------------------------------------------------------

heman_image* hut_read_image(const char* filename, int nbands)
{
	int width = 0, height = 0;
	stbi_uc* bytes;
	heman_image* retval;
	bytes = stbi_load(filename, &width, &height, nullptr, nbands);
	if (bytes == nullptr) { return nullptr; }
	retval = heman_import_u8(width, height, nbands, bytes, 0, 1);
	stbi_image_free(bytes);
	return retval;
}

struct SdfInterpolator
{
	// Input:
	int   _num_frames                 = 8;
	float _data_weight                = 1;
	float _xy_smoothness_weight       = 0.001;
	float _temporal_smoothness_weight = 0.001;
	float _c2_smoothness              = 0.001;

	// Output:
	int _width;
	int _height;
	std::vector<float> _solution;

	// View:
	int _frame = 12;

	gl::Texture _sdf_texture{"sdf", gl::TexParams::clamped_nearest()};
	gl::Texture _blob_texture{"blob", gl::TexParams::clamped_nearest()};

	SdfInterpolator()
	{
		calc();
	}

	/// z = time/interpolation
	int index(int x, int y, int z) const
	{
		return x + _width * (y + _height * z);
	}

	void calc()
	{
		LOG_SCOPE_F(INFO, "SdfInterpolator::calc");
		heman_image* seed_a = hut_read_image("sdf_interpolation/a.png", 1);
		CHECK_NOTNULL_F(seed_a, "Failed to load image.");
		heman_image* sdf_a = heman_distance_create_sdf(seed_a);
		heman_image* cpcf_a = heman_distance_create_cpcf(seed_a);
		SCOPE_EXIT{ heman_image_destroy(sdf_a); };
		SCOPE_EXIT{ heman_image_destroy(cpcf_a); };
		heman_image_destroy(seed_a);

		heman_image* seed_b = hut_read_image("sdf_interpolation/b.png", 1);
		CHECK_NOTNULL_F(seed_b, "Failed to load image.");
		heman_image* sdf_b = heman_distance_create_sdf(seed_b);
		heman_image* cpcf_b = heman_distance_create_cpcf(seed_b);
		SCOPE_EXIT{ heman_image_destroy(sdf_b); };
		SCOPE_EXIT{ heman_image_destroy(cpcf_b); };
		heman_image_destroy(seed_b);

		int bands;
		heman_image_info(sdf_a, &_width, &_height, &bands);
		CHECK_EQ_F(bands, 1);

		fi::LinearEquation eq;

		for (int y : emilib::irange(0, _height)) {
			for (int x : emilib::irange(0, _width)) {
				add_equation(&eq, fi::Weight{_data_weight}, fi::Rhs{*heman_image_texel(sdf_a, x, y)}, {
					{index(x, y, 0), 1.0f}
				});
				add_equation(&eq, fi::Weight{_data_weight}, fi::Rhs{*heman_image_texel(sdf_b, x, y)}, {
					{index(x, y, _num_frames - 1), 1.0f}
				});
			}
		}

		for (int z : emilib::irange(0, _num_frames)) {
			bool is_anchor_frame = (z == 0 || z + 1 == _num_frames);
			for (int y : emilib::irange(0, _height)) {
				for (int x : emilib::irange(0, _width)) {
					add_equation(&eq, fi::Weight{1e-6f}, fi::Rhs{0.0f}, {{index(x,y,z), 1.0f}});
					if (!is_anchor_frame) {
						if (x + 2 < _width) {
							add_equation(&eq, fi::Weight{_xy_smoothness_weight}, fi::Rhs{0.0f}, {
								{index(x + 0, y, z), +1.0f},
								{index(x + 1, y, z), -2.0f},
								{index(x + 2, y, z), +1.0f},
							});
						}

						if (y + 2 < _height) {
							add_equation(&eq, fi::Weight{_xy_smoothness_weight}, fi::Rhs{0.0f}, {
								{index(x, y + 0, z), +1.0f},
								{index(x, y + 1, z), -2.0f},
								{index(x, y + 2, z), +1.0f},
							});
						}
					}

					if (z + 2 < _num_frames) {
						add_equation(&eq, fi::Weight{_temporal_smoothness_weight}, fi::Rhs{0.0f}, {
							{index(x, y, z + 0), +1.0f},
							{index(x, y, z + 1), -2.0f},
							{index(x, y, z + 2), +1.0f},
						});
					}

					// We want to detect and propagate "creases"
					// They can stay put or slide around, so:
					if (z + 1 < _num_frames) {
						if (x + 4 < _width) {
							add_equation(&eq, fi::Weight{_temporal_smoothness_weight}, fi::Rhs{0.0f}, {
								{index(x + 1, y, z), +1.0f * -1.0f},
								{index(x + 2, y, z), -2.0f * -1.0f},
								{index(x + 3, y, z), +1.0f * -1.0f},

								{index(x + 0, y, z + 1), +1.0f},
								{index(x + 1, y, z + 1), -2.0f},
								{index(x + 2, y, z + 1), +1.0f},
								{index(x + 1, y, z + 1), +1.0f},
								{index(x + 2, y, z + 1), -2.0f},
								{index(x + 3, y, z + 1), +1.0f},
								{index(x + 2, y, z + 1), +1.0f},
								{index(x + 3, y, z + 1), -2.0f},
								{index(x + 4, y, z + 1), +1.0f},
							});
						}
						if (y + 4 < _height) {
							add_equation(&eq, fi::Weight{_temporal_smoothness_weight}, fi::Rhs{0.0f}, {
								{index(x, y + 1, z), +1.0f * -1.0f},
								{index(x, y + 2, z), -2.0f * -1.0f},
								{index(x, y + 3, z), +1.0f * -1.0f},

								{index(x, y + 0, z + 1), +1.0f},
								{index(x, y + 1, z + 1), -2.0f},
								{index(x, y + 2, z + 1), +1.0f},
								{index(x, y + 1, z + 1), +1.0f},
								{index(x, y + 2, z + 1), -2.0f},
								{index(x, y + 3, z + 1), +1.0f},
								{index(x, y + 2, z + 1), +1.0f},
								{index(x, y + 3, z + 1), -2.0f},
								{index(x, y + 4, z + 1), +1.0f},
							});
						}
					}

					// if (z + 1 < _num_frames) {
					// 	if (x + 3 < _width) {
					// 		add_equation(&eq, fi::Weight{_c2_smoothness}, fi::Rhs{0.0f}, {
					// 			{index(x + 0, y, z + 0), +1.0f},
					// 			{index(x + 1, y, z + 0), -3.0f},
					// 			{index(x + 2, y, z + 0), +3.0f},
					// 			{index(x + 3, y, z + 0), -1.0f},

					// 			{index(x + 0, y, z + 1), +1.0f * -1.0f},
					// 			{index(x + 1, y, z + 1), -3.0f * -1.0f},
					// 			{index(x + 2, y, z + 1), +3.0f * -1.0f},
					// 			{index(x + 3, y, z + 1), -1.0f * -1.0f},
					// 		});
					// 	}
					// 	if (y + 3 < _height) {
					// 		add_equation(&eq, fi::Weight{_c2_smoothness}, fi::Rhs{0.0f}, {
					// 			{index(x, y + 0, z + 0), +1.0f},
					// 			{index(x, y + 1, z + 0), -3.0f},
					// 			{index(x, y + 2, z + 0), +3.0f},
					// 			{index(x, y + 3, z + 0), -1.0f},

					// 			{index(x, y + 0, z + 1), +1.0f * -1.0f},
					// 			{index(x, y + 1, z + 1), -3.0f * -1.0f},
					// 			{index(x, y + 2, z + 1), +3.0f * -1.0f},
					// 			{index(x, y + 3, z + 1), -1.0f * -1.0f},
					// 		});
					// 	}
					// }

					// if (z + 1 < _num_frames) {
					// 	if (x + 4 < _width) {
					// 		add_equation(&eq, fi::Weight{_c2_smoothness}, fi::Rhs{0.0f}, {
					// 			{index(x + 0, y, z + 0), +1.0f},
					// 			{index(x + 1, y, z + 0), -4.0f},
					// 			{index(x + 2, y, z + 0), +6.0f},
					// 			{index(x + 3, y, z + 0), -4.0f},
					// 			{index(x + 4, y, z + 0), +1.0f},

					// 			{index(x + 0, y, z + 1), +1.0f * -1.0f},
					// 			{index(x + 1, y, z + 1), -4.0f * -1.0f},
					// 			{index(x + 2, y, z + 1), +6.0f * -1.0f},
					// 			{index(x + 3, y, z + 1), -4.0f * -1.0f},
					// 			{index(x + 4, y, z + 1), +1.0f * -1.0f},
					// 		});
					// 	}
					// 	if (y + 4 < _height) {
					// 		add_equation(&eq, fi::Weight{_c2_smoothness}, fi::Rhs{0.0f}, {
					// 			{index(x, y + 0, z + 0), +1.0f},
					// 			{index(x, y + 1, z + 0), -4.0f},
					// 			{index(x, y + 2, z + 0), +6.0f},
					// 			{index(x, y + 3, z + 0), -4.0f},
					// 			{index(x, y + 4, z + 0), +1.0f},

					// 			{index(x, y + 0, z + 1), +1.0f * -1.0f},
					// 			{index(x, y + 1, z + 1), -4.0f * -1.0f},
					// 			{index(x, y + 2, z + 1), +6.0f * -1.0f},
					// 			{index(x, y + 3, z + 1), -4.0f * -1.0f},
					// 			{index(x, y + 4, z + 1), +1.0f * -1.0f},
					// 		});
					// 	}
					// }
				}
			}
		}

		const int num_unknowns = _width * _height * _num_frames;
		LOG_SCOPE_F(INFO, "solve_sparse_linear %dx%dx%d = %d unknowns, %lu equations",
			_width, _height, _num_frames, num_unknowns, eq.rhs.size());
		_solution = solve_sparse_linear(num_unknowns, eq.triplets, eq.rhs);
		if (_solution.size() != num_unknowns) {
			_solution.resize(num_unknowns, 0.0f);
		}
	}

	void show_input()
	{
		bool changed = false;
		changed |= ImGui::SliderInt("Num frames", &_num_frames, 3, 128);
		changed |= ImGui::SliderFloat("_data_weight", &_data_weight, 0, 1000, "%.3f", 4);
		changed |= ImGui::SliderFloat("_xy_smoothness_weight", &_xy_smoothness_weight, 0, 1000, "%.3f", 4);
		changed |= ImGui::SliderFloat("_temporal_smoothness_weight", &_temporal_smoothness_weight, 0, 1000, "%.3f", 4);
		changed |= ImGui::SliderFloat("_c2_smoothness", &_c2_smoothness, 0, 1000, "%.3f", 4);
		if (changed) {
			calc();
		}
	}

	void show()
	{
		show_input();
		_frame = math::clamp(_frame, 0, _num_frames - 1);
		ImGui::SliderInt("Frame", &_frame, 0, _num_frames - 1);

		std::vector<RGBA> sdf_image;
		std::vector<RGBA> blob_image;

		float max_abs_dist = 1e-6f;
		for (int y : emilib::irange(0, _height)) {
			for (int x : emilib::irange(0, _width)) {
				float dist = _solution[index(x, y, _frame)];
				max_abs_dist = std::max(max_abs_dist, std::abs(dist));
			}
		}

		for (int y : emilib::irange(0, _height)) {
			for (int x : emilib::irange(0, _width)) {
				float dist = _solution[index(x, y, _frame)];
				const uint8_t dist_u8 = std::min<float>(255, 255 * std::abs(dist) / max_abs_dist);
				const uint8_t inv_dist_u8 = 255 - dist_u8;;
				if (dist < 0) {
					sdf_image.emplace_back(RGBA{inv_dist_u8, inv_dist_u8, 255, 255});
				} else {
					sdf_image.emplace_back(RGBA{255, inv_dist_u8, inv_dist_u8, 255});
				}

				float insideness = 1 - std::max(0.0, std::min(1.0, (127 * dist + 0.5) * 2));
				const uint8_t color = 255 * insideness;
				blob_image.emplace_back(RGBA{color, color, color, 255});
			}
		}

		ImGui::Text("max_abs_dist: %f", max_abs_dist);

		const auto image_size = gl::Size{static_cast<unsigned>(_width), static_cast<unsigned>(_height)};
		_sdf_texture.set_data(sdf_image.data(), image_size, gl::ImageFormat::RGBA32);
		_blob_texture.set_data(blob_image.data(), image_size, gl::ImageFormat::RGBA32);

		ImVec2 canvas_size(320, 230);
		ImGui::Image(reinterpret_cast<ImTextureID>(_sdf_texture.id()), canvas_size);
		ImGui::SameLine();
		ImGui::Image(reinterpret_cast<ImTextureID>(_blob_texture.id()), canvas_size);
	}

};

// ----------------------------------------------------------------------------

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

	FieldGui field_gui;
	Field1DInput field_1d_input = load_1d_field();

	SdfInterpolator sdf_interpolation;

	bool quit = false;
	while (!quit) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) { quit = true; }
			imgui_sdl.on_event(event);
		}
		gl::TempViewPort::set_back_buffer_size(
			math::round_to_int(imgui_sdl.width_pixels()),
			math::round_to_int(imgui_sdl.height_pixels()));
		imgui_sdl.new_frame();

		ImGui::ShowTestWindow();

		if (ImGui::Begin("1D field interpolation")) {
			show_1d_field_window(&field_1d_input);
		}
		ImGui::End();

		if (ImGui::Begin("1D sine denoiser")) {
			show_1d_denoiser_window();
		}
		ImGui::End();

		if (ImGui::Begin("2D field interpolation")) {
			show_2d_field_window();
		}
		ImGui::End();

		if (ImGui::Begin("SDF interpolation")) {
			sdf_interpolation.show();
		}
		ImGui::End();

		show_sdf_fields(&field_gui);

		glClearColor(0.1f, 0.1f, 0.1f, 0);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		imgui_sdl.paint();

		SDL_GL_SwapWindow(sdl.window);
	}
}
