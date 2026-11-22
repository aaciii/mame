// SPDX-License-Identifier: BSD-3-Clause
class menu_select_launch::video_preview
{
public:
    explicit video_preview(render_manager &render)
        : texture(render.texture_alloc(render_texture::hq_scale), render) { }
    ~video_preview() { player.stop(); }
    videosnap::player player;
    bitmap_argb32 still_bitmap;
    bitmap_argb32 frame_bitmap;
    texture_ptr texture;
    std::string key;
    bool displaying_video = false;
    bool bound = false;
    double display_aspect = 1.0;
};

bool menu_select_launch::needs_continuous_update() const
{
    return is_active() && show_right_panel() && (m_right_panel == RP_VIDEO);
}

void menu_select_launch::stop_video_preview()
{
    m_video.reset();
}

void menu_select_launch::video_render(u32 flags)
{
    draw_right_box_heading(flags, false, false, _("Video"));
    if (!is_active() || (flags & PROCESS_NOINPUT))
    {
        stop_video_preview();
        return;
    }
    ui_software_info const *software;
    ui_system_info const *system;
    get_selection(software, system);
    game_driver const *driver = system ? system->driver : (software ? software->driver : nullptr);
    if (!driver)
    {
        stop_video_preview();
        return;
    }
    bool const is_software = software && !software->startempty;
    std::string const relative = is_software
        ? util::path_concat(software->listname, software->shortname)
        : std::string(driver->name);
    std::string const key = std::string(driver->name) + ":" + relative;
    if (!m_video || (m_video->key != key))
    {
        stop_video_preview();
        m_video = std::make_unique<video_preview>(machine().render());
        m_video->key = key;
        // Snapshot paths, but not the Images cache or missing-artwork placeholder.
        if (ui().options().video_image_fallback())
        {
            std::string snapshot_path = ui().options().video_image_fallback_directory();
            if (snapshot_path.empty())
            {
                u8 const previous_view = m_image_view;
                m_image_view = SNAPSHOT_VIEW;
                snapshot_path = get_arts_searchpath();
                m_image_view = previous_view;
            }
            emu_file snapfile(snapshot_path, OPEN_FLAG_READ);
            if (is_software)
            {
                load_image(m_video->still_bitmap, snapfile, relative);
                if (!m_video->still_bitmap.valid())
                    load_image(m_video->still_bitmap, snapfile,
                        util::path_concat(software->driver->name + software->part, software->shortname));
            }
            else
                load_driver_image(m_video->still_bitmap, snapfile, *driver, ui().options().video_inherit_parent());
        }
        std::string filename = videosnap::local_video_path(relative + ".mp4", ui().options().video_directory());
        if (filename.empty() && ui().options().video_inherit_parent())
        {
            if (is_software)
            {
                if (!software->parentname.empty())
                    filename = videosnap::local_video_path(
                        util::path_concat(software->listname, software->parentname) + ".mp4",
                        ui().options().video_directory());
            }
            else
            {
                int const parent = driver_list::non_bios_clone(*driver);
                if (parent >= 0)
                    filename = videosnap::local_video_path(
                        std::string(driver_list::driver(parent).name) + ".mp4",
                        ui().options().video_directory());
            }
        }
        if (!filename.empty())
        {
            m_video->player.configure_audio(ui().options().video_enable_audio(),
                ui().options().video_volume());
            m_video->player.start(filename);
        }
    }
    auto &preview = *m_video;
    videosnap::frame next;
    if (preview.player.take_frame(next))
    {
        preview.frame_bitmap.allocate(next.width, next.height);
        for (int y = 0; y < next.height; ++y)
            std::copy_n(next.argb.data() + std::size_t(y) * next.width,
                next.width, &preview.frame_bitmap.pix(y));
        preview.texture->set_bitmap(preview.frame_bitmap,
            preview.frame_bitmap.cliprect(), TEXFORMAT_ARGB32);
        preview.display_aspect = next.display_aspect;
        preview.displaying_video = true;
        preview.bound = true;
    }
    if (preview.player.status() == videosnap::state::failed)
    {
        if (preview.displaying_video)
            preview.bound = false;
        preview.displaying_video = false;
    }
    if (!preview.displaying_video)
    {
        if (!preview.still_bitmap.valid())
            return;
        if (!preview.bound)
        {
            preview.texture->set_bitmap(preview.still_bitmap,
                preview.still_bitmap.cliprect(), TEXFORMAT_ARGB32);
            preview.bound = true;
        }
        preview.display_aspect = double(preview.still_bitmap.width()) / preview.still_bitmap.height();
    }
    float const available_width = m_right_content_hbounds.second - m_right_content_hbounds.first;
    float const available_height = m_right_content_vbounds.second - m_right_content_vbounds.first;
    float const ratio = float(preview.display_aspect) * x_aspect();
    if ((available_width <= 0) || (available_height <= 0) || !(ratio > 0))
        return;
    float width = available_width;
    float height = available_height;
    if ((width / height) > ratio)
        width = height * ratio;
    else
        height = width / ratio;
    float const left = m_right_content_hbounds.first + (available_width - width) * 0.5F;
    float const top = m_right_content_vbounds.first + (available_height - height) * 0.5F;
    container().add_quad(left, top, left + width, top + height, rgb_t::white(),
        preview.texture.get(), PRIMFLAG_BLENDMODE(BLENDMODE_ALPHA));
}
